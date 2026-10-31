// SPDX-License-Identifier: GPL-2.0
// Standalone host-buffer integrity/ordering check. No GPU or kernel changes.
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
using Clock = std::chrono::steady_clock;
static void require(bool value, const char* what) {
  if (!value) throw std::runtime_error(what);
}
static uint64_t pattern(uint64_t seq, uint64_t word) {
  uint64_t x = (seq + 1) * 0x9e3779b97f4a7c15ULL + word * 0xd1b54a32d192ed03ULL;
  x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27;
  return x ^ (x >> 31);
}
static bool valid(const uint64_t* p, size_t words, uint64_t seq) {
  for (size_t i = 0; i < words; ++i) if (p[i] != pattern(seq, i)) return false;
  return true;
}
struct Wire {
  uint64_t address{};
  uint32_t rkey{}, qpn{}, depth{}, bytes{}, rounds{}, version{3}, overlap{}, alias_rkey{};
  uint64_t alias_address{};
  uint8_t gid[16]{};
};
struct Bench {
  int rank{}, depth{}, rounds{}, sock{-1}; size_t bytes{}; bool overlap{}, partial{}, alias{};size_t allocation{};int memfd{-1};void* alias_buffer{};ibv_mr* alias_mr{};
  ibv_context* ctx{}; ibv_pd* pd{}; ibv_cq* cq{}; ibv_qp* qp{}; ibv_mr* mr{};
  uint64_t* buffer{}; Wire peer{};
  ~Bench() {
    if (qp && ibv_destroy_qp(qp)) _exit(8);
    if (alias_mr && ibv_dereg_mr(alias_mr)) _exit(8);
    if (mr && ibv_dereg_mr(mr)) _exit(8);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (ctx) ibv_close_device(ctx);
    if(alias_buffer) munmap(alias_buffer,allocation);
    if(memfd>=0){if(buffer)munmap(buffer,allocation);::close(memfd);}
    else std::free(buffer);
    if (sock >= 0) ::close(sock);
  }
  void io(void* p, size_t n, bool write) {
    auto* q = static_cast<char*>(p);
    while (n) {
      const auto k = write ? ::send(sock, q, n, MSG_NOSIGNAL) : ::recv(sock, q, n, 0);
      require(k > 0, "control channel"); q += k; n -= k;
    }
  }
  void exchange(void* mine, void* theirs, size_t n) {
    if (!rank) { io(mine, n, true); io(theirs, n, false); }
    else { io(theirs, n, false); io(mine, n, true); }
  }
  void setup(const char* host, int port) {
    int count{}; auto** devices = ibv_get_device_list(&count);
    require(devices, "enumerate devices");
    for (int i=0; i<count; ++i)
      if (std::strcmp(ibv_get_device_name(devices[i]), (std::getenv("STRIX_RDMA_DEVICE") ? std::getenv("STRIX_RDMA_DEVICE") : "usb4_rdma0")) == 0)
        ctx = ibv_open_device(devices[i]);
    ibv_free_device_list(devices); require(ctx, "open usb4_rdma0");
    pd=ibv_alloc_pd(ctx); require(pd, "allocate PD");
    allocation=(!rank && overlap && !partial)?bytes:bytes*depth;
    if(!rank && alias){
      memfd=syscall(SYS_memfd_create,"rdma-alias-check",MFD_CLOEXEC);require(memfd>=0,"create shared backing");
      require(!ftruncate(memfd,allocation),"size shared backing");
      void* first=mmap(nullptr,allocation,PROT_READ|PROT_WRITE,MAP_SHARED,memfd,0);require(first!=MAP_FAILED,"map first alias");buffer=static_cast<uint64_t*>(first);
      void* second=mmap(nullptr,allocation,PROT_READ|PROT_WRITE,MAP_SHARED,memfd,0);require(second!=MAP_FAILED,"map second alias");alias_buffer=second;
      alias_mr=ibv_reg_mr(pd,alias_buffer,allocation,IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE);require(alias_mr,"register alias MR");
    }else require(!posix_memalign(reinterpret_cast<void**>(&buffer),4096,allocation),"allocate buffer");
    std::memset(buffer,0,allocation);
    mr=ibv_reg_mr(pd,buffer,allocation,IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE);
    require(mr,"register fixed buffer");
    cq=ibv_create_cq(ctx,depth*2+16,nullptr,nullptr,0); require(cq,"create CQ");
    ibv_qp_init_attr init{}; init.send_cq=cq;init.recv_cq=cq;init.qp_type=IBV_QPT_RC;
    init.cap.max_send_wr=depth;init.cap.max_recv_wr=depth;init.cap.max_send_sge=1;init.cap.max_recv_sge=1;
    qp=ibv_create_qp(pd,&init);require(qp,"create QP");
    ibv_qp_attr a{};a.qp_state=IBV_QPS_INIT;a.port_num=1;
    a.qp_access_flags=IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE;
    require(!ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS),"QP INIT");
    sock=::socket(AF_INET,SOCK_STREAM,0);require(sock>=0,"socket");int yes=1;
    setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    setsockopt(sock,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
    sockaddr_in ad{};ad.sin_family=AF_INET;ad.sin_port=htons(port);
    require(inet_pton(AF_INET,host,&ad.sin_addr)==1,"control IP");
    if (!rank) {
      require(!bind(sock,reinterpret_cast<sockaddr*>(&ad),sizeof(ad)),"bind");require(!listen(sock,1),"listen");
      int s=accept(sock,nullptr,nullptr);require(s>=0,"accept");::close(sock);sock=s;
    } else {
      const auto deadline=Clock::now()+std::chrono::seconds(10);
      while (connect(sock,reinterpret_cast<sockaddr*>(&ad),sizeof(ad))) {
        require(Clock::now()<deadline,"connect timeout");std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    timeval timeout{15,0};setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));setsockopt(sock,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
    ibv_gid gid{};require(!ibv_query_gid(ctx,1,1,&gid),"GID1");
    Wire mine{};mine.address=reinterpret_cast<uint64_t>(buffer);mine.rkey=mr->rkey;mine.qpn=qp->qp_num;
    mine.depth=depth;mine.bytes=bytes;mine.rounds=rounds;mine.overlap=alias?3:partial?2:overlap?1:0;
    if(alias_mr){mine.alias_address=reinterpret_cast<uint64_t>(alias_buffer);mine.alias_rkey=alias_mr->rkey;}std::memcpy(mine.gid,&gid,16);
    exchange(&mine,&peer,sizeof(mine));require(peer.version==3 && peer.overlap==mine.overlap && peer.depth==mine.depth && peer.bytes==mine.bytes && peer.rounds==mine.rounds,"matched configuration");
    a={};a.qp_state=IBV_QPS_RTR;a.path_mtu=IBV_MTU_4096;a.dest_qp_num=peer.qpn;a.rq_psn=0;
    a.max_dest_rd_atomic=1;a.min_rnr_timer=12;a.ah_attr.port_num=1;a.ah_attr.is_global=1;
    std::memcpy(&a.ah_attr.grh.dgid,peer.gid,16);a.ah_attr.grh.sgid_index=1;a.ah_attr.grh.hop_limit=1;
    require(!ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER),"QP RTR");
    a={};a.qp_state=IBV_QPS_RTS;a.timeout=14;a.retry_cnt=7;a.rnr_retry=7;a.max_rd_atomic=1;
    require(!ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC),"QP RTS");
  }
  ibv_wc completion() {
    const auto deadline=Clock::now()+std::chrono::seconds(10);ibv_wc wc{};
    for (uint64_t spins=0;;++spins) {
      int n=ibv_poll_cq(cq,1,&wc);require(n>=0,"poll CQ");
      if(n) {if(wc.status!=IBV_WC_SUCCESS)throw std::runtime_error("WC status="+std::to_string(wc.status)+" wr_id="+std::to_string(wc.wr_id));return wc;}
      require(Clock::now()<deadline,"CQ timeout");if(!(spins%64))std::this_thread::yield();
    }
  }
  void run() {
    for (int batch=0;batch<rounds;++batch) {
      // All buffers are retained until peer validation and local completions.
      // The LAN handshake is a batch reuse fence, not a transport latency test.
      if (!rank) for(int j=0;j<depth;++j) {
        ibv_recv_wr wr{};wr.wr_id=j;ibv_recv_wr* bad{};
        require(!ibv_post_recv(qp,&wr,&bad),"prepost receive");
      }
      uint32_t ready=batch,got{};exchange(&ready,&got,sizeof(ready));require(got==ready,"batch readiness");
      if (rank) {
        for(int j=0;j<depth;++j) {
          uint64_t seq=uint64_t(batch)*depth+j;auto* slot=buffer+j*(bytes/8);
          for(size_t k=0;k<bytes/8;++k)slot[k]=pattern(seq,k);
        }
        for(int j=0;j<depth;++j) {
          uint64_t seq=uint64_t(batch)*depth+j;auto* slot=buffer+j*(bytes/8);
          ibv_sge sg{reinterpret_cast<uint64_t>(slot),static_cast<uint32_t>(bytes),mr->lkey};
          ibv_send_wr wr{};wr.wr_id=seq;wr.sg_list=&sg;wr.num_sge=1;wr.opcode=IBV_WR_RDMA_WRITE_WITH_IMM;
          wr.send_flags=IBV_SEND_SIGNALED;wr.imm_data=htonl(static_cast<uint32_t>(seq));
          wr.wr.rdma.remote_addr=peer.address+(partial?j*(bytes/2):overlap?0:j*bytes);wr.wr.rdma.rkey=peer.rkey;
          if(alias && (j%2)){require(peer.alias_address && peer.alias_rkey,"peer alias registration");wr.wr.rdma.remote_addr=peer.alias_address;wr.wr.rdma.rkey=peer.alias_rkey;}ibv_send_wr* bad{};
          const int post_result=ibv_post_send(qp,&wr,&bad);
          if(post_result) {
            const int saved_errno=errno;ibv_qp_attr observed{};ibv_qp_init_attr observed_init{};
            const int query=ibv_query_qp(qp,&observed,IBV_QP_STATE,&observed_init);
            throw std::runtime_error("post write with immediate result="+std::to_string(post_result)+" errno="+std::to_string(saved_errno)+" seq="+std::to_string(seq)+" query_result="+std::to_string(query)+" qp_state="+std::to_string(observed.qp_state));
          }
        }
        for(int j=0;j<depth;++j) {
          auto wc=completion();require(wc.opcode==IBV_WC_RDMA_WRITE && wc.wr_id==uint64_t(batch)*depth+j,"send completion ordering");
        }
      } else {
        for(int j=0;j<depth;++j) {
          uint64_t seq=uint64_t(batch)*depth+j;auto wc=completion();
          require(wc.opcode==IBV_WC_RECV_RDMA_WITH_IMM && (wc.wc_flags&IBV_WC_WITH_IMM),"receive opcode");
          require(wc.wr_id==static_cast<uint64_t>(j) && ntohl(wc.imm_data)==seq,"receive completion ordering/identity");
          std::atomic_thread_fence(std::memory_order_acquire);
          if(!overlap && !partial && !valid(buffer+j*(bytes/8),bytes/8,seq))throw std::runtime_error("payload mismatch seq="+std::to_string(seq));
        }
      }
      if (!rank && (overlap || partial)) {
        std::atomic_thread_fence(std::memory_order_acquire);
        if(partial){
          const size_t half=bytes/16,total=(depth+1)*half;
          for(size_t k=0;k<total;++k){size_t writer=std::min(k/half,size_t(depth-1));require(buffer[k]==pattern(uint64_t(batch)*depth+writer,k-writer*half),"partial overlap final region");}
        }else require(valid(buffer,bytes/8,uint64_t(batch+1)*depth-1),"overlapping/alias final payload");
      }
      uint64_t ack=uint64_t(batch+1)*depth,other{};exchange(&ack,&other,sizeof(ack));require(ack==other,"validated reuse fence");
    }
    std::printf("verified rank=%d depth=%d bytes=%zu rounds=%d messages=%llu words=%llu ordering=pass reuse=pass mode=%s payloads_verified=%llu\n",rank,depth,bytes,rounds,static_cast<unsigned long long>(uint64_t(rounds)*depth),static_cast<unsigned long long>(uint64_t(rounds)*depth*(bytes/8)),alias?"physical-alias-final":partial?"partial-final":overlap?"overlap-final":"disjoint-all",static_cast<unsigned long long>(rank?0:((overlap||partial)?rounds:uint64_t(rounds)*depth)));
  }
};
int main(int argc,char**argv) {
  try {
    if(argc==2 && std::string(argv[1])=="--self-test") {
      uint64_t v[16];for(size_t i=0;i<16;++i)v[i]=pattern(7,i);
      require(valid(v,16,7),"pattern self-test");require(!valid(v,16,8),"stale sequence detection");
      v[9]^=1;require(!valid(v,16,7),"payload mutation detection");puts("Pattern identity/corruption checks passed");return 0;
    }
    require(argc==7 || argc==8,"usage: pipeline-check rank host port depth bytes rounds [disjoint|overlap|partial|alias]");Bench b;
    if(argc==8){require(std::string(argv[7])=="disjoint" || std::string(argv[7])=="overlap" || std::string(argv[7])=="partial" || std::string(argv[7])=="alias","mode");b.partial=std::string(argv[7])=="partial";b.alias=std::string(argv[7])=="alias";b.overlap=std::string(argv[7])!="disjoint";}
    b.rank=std::stoi(argv[1]);b.depth=std::stoi(argv[4]);b.bytes=std::stoull(argv[5]);b.rounds=std::stoi(argv[6]);
    require((b.rank==0||b.rank==1)&&b.depth>=1&&b.depth<=128&&b.bytes>=8&&b.bytes<=41943104&&b.bytes%8==0&&b.rounds>=1&&b.rounds<=8192,"argument bounds");
    b.setup(argv[2],std::stoi(argv[3]));b.run();return 0;
  } catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 2;}
}
