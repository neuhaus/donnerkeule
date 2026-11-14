# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from production import source_file, functions


class ProgressReadyTests(unittest.TestCase):
    def test_actual_next_descriptor_check_handles_wrap_control_and_lifecycle(self):
        here=Path(__file__).resolve().parent
        source = source_file('kernel/verbs_dmabuf.c')
        first=source.index('static bool tbv_dmabuf_ring_ready(')
        helper=source[first:source.index('/* Hop zero belongs',first)]
        harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define le32_to_cpu(x) (x)
#define RING_DESC_COMPLETED 2
#define PCI_VENDOR_ID_AMD 0x1022
#define TBV_BULK_NHI_ABI 1
struct list_head {int nonempty;};
static bool list_empty(struct list_head *p){return !p->nonempty;}
struct pci_dev {unsigned vendor,device;bool pci;};
struct nhi {struct pci_dev *dev;bool going_away;};
struct tbv_bulk_desc {uint64_t address;u32 metadata,timestamp;};
struct tb_ring {struct nhi *nhi;int lock,size,head,tail;bool running,start_poll;void *descriptors;struct list_head queue;};
static bool dmabuf_batch_doorbell=true;
static bool dev_is_pci(struct pci_dev *p){return p->pci;}
static struct pci_dev *to_pci_dev(struct pci_dev *p){return p;}
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!*(p));*(p)=1;}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);assert(*(p));*(p)=0;}while(0)
#include "ready.c"
int main(void){
 struct pci_dev d={.vendor=0x1022,.device=0x158d,.pci=true};struct nhi n={.dev=&d};
 struct tbv_bulk_desc desc[10]={0};struct tb_ring r={.nhi=&n,.size=10,.running=true,.descriptors=desc};
 assert(!tbv_dmabuf_ring_ready(&r));r.queue.nonempty=1;assert(tbv_dmabuf_ring_ready(&r));r.queue.nonempty=0;
 for(int tail=0;tail<10;tail++){
  memset(desc,0,sizeof(desc));r.tail=tail;r.head=(tail+3)%10;
  desc[(tail+1)%10].metadata=2u<<20;assert(!tbv_dmabuf_ring_ready(&r));
  desc[tail].metadata=2u<<20;assert(tbv_dmabuf_ring_ready(&r));assert(!r.lock);
 }
 r.running=false;assert(!tbv_dmabuf_ring_ready(&r));r.running=true;
 n.going_away=true;assert(!tbv_dmabuf_ring_ready(&r));n.going_away=false;
 r.tail=10;assert(tbv_dmabuf_ring_ready(&r));r.tail=-1;assert(tbv_dmabuf_ring_ready(&r));r.tail=0;
 r.descriptors=NULL;assert(tbv_dmabuf_ring_ready(&r));r.descriptors=desc;
 r.size=1;assert(tbv_dmabuf_ring_ready(&r));r.size=10;
 r.head=r.tail=0;r.start_poll=true;assert(tbv_dmabuf_ring_ready(&r));r.start_poll=false;
 dmabuf_batch_doorbell=false;assert(tbv_dmabuf_ring_ready(&r));dmabuf_batch_doorbell=true;
 d.pci=false;assert(tbv_dmabuf_ring_ready(&r));d.pci=true;
 d.device=0xffff;assert(tbv_dmabuf_ring_ready(&r));d.device=0x158e;assert(!tbv_dmabuf_ring_ready(&r));
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'ready.c').write_text(helper);(root/'test.c').write_text(harness)
            result=subprocess.run([shutil.which('cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                '-I',str(root),str(root/'test.c'),'-o',str(root/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(root/'test')],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
