#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "data.h"
#include "lib.h"
#include "ixgbe.h"
/*
NICへの割り込み禁止
デバイスのリセットとConfigurationレジスタの設定
DMAの初期化を待つ
PHYとLINKの設定
receiveの初期化
transmitの初期化
割り込みを許可
*/

const char *driver_name = "ixgbe82599";
const int MAX_RX_QUEUE_ENTRIES = 4096;
const int MAX_TX_QUEUE_ENTRIES = 4096;

const int NUM_RX_QUEUE_ENTRIES = 512;
const int NUM_RX_QUEUE_ENTRIES = 512;

const int PKT_BUF_ENTRY_SIZE = 2048;
const int MIN_MEMPOOL_ENTRIES = 4096;

const int TX_CLEAN_BATCH = 32;

struct tx_queue{
    volatile union ixgbe_adv_tx_desc *descriptors;
    struct mempool *mempool;
    uint16_t num_entries;
    uint16_t clean_index;
    uint16_t tx_index;
    void *virtual_address[];
}

struct rx_queue{
    volatile union ixgbe_adv_rx_desc *descriptors;
    struct mempool *mempool
    uint16_t num_entries;
    uint16_t rx_index;
    void *virtual_address[];
}

//PHYとLINKの設定
void init_link(struct ixgbe_device *ix_dev)
{
    printf("start: init_link");
    //BX/SGMII LINKのセットアップ
    //AUTOCのなかのLink mode select fieldを適切な運用モードに
    set_reg32(ix_dev->addr,IXGBE_AUTOC,(get_reg32(ix_dev->addr,IXGBE_AUTOC) & ~IXGBE_AUTOC_LMS_MASK) | IXGBE_AUTOC_LMS_10G_SERIAL);
    set_reg32(ix_dev->addr,IXGBE_AUTOC,(get_reg32(ix_dev->addr,IXGBE_AUTOC) & ~IXGBE_AUTOC_10G_PMA_PMF_MASK) | IXGBE_AUTOC_10G_XAUI);
    set_flag32(ix_dev->addr,IXGBE_AUTOC,IXGBE_AUTOC_AN_RESTART);
    printf("end: init_link");
}

//特定のレジスタをreadすると初期化されるっぽい？？？
void init_stats(struct ixgbe_device *ix_dev)
{
    printf("start: init_stats");
    //good packet receive and transmit
    uint32_t rx_pkts = get_reg32(ix_dev->addr,IXGBE_GPRC);
    uint32_t tx_pkts = get_reg32(ix_dev->addr,IXGBE_GPTC);
    //good octet receive and transmit
    uint64_t rx_bytes = get_reg32(ix_dev->addr,IXGBE_GORCL) + (((uint64_t)get_reg32(ix_dev->addr,IXGBE_GORCH)) << 32);
    uint64_t tx_bytes = get_reg32(ix_dev->addr,IXGBE_GOTCL) + (((uint64_t)get_reg32(ix_dev->addr,IXGBE_GOTCH)) << 32);
    printf("end: init_stats");
}

void init_rx_reg(struct ixgbe_device *ix_dev)
{
    int i;
    printf("start: init_rx_reg");
    set_reg32(ix_dev->addr,IXGBE_RXPBSIZE(0),IXGBE_RXPBSIZE_128KB);
    for(i=0;i<8;i++){
            set_reg32(ix_dev->addr,IXGBE_PXPBSIZE(i),0);
    }

    set_flag32(ix_dev->addr,IXGBE_HLREG0,IXGBE_HLREG0_RXCRCSTRP);
    set_flag32(ix_dev->addr,IXGBE_RDRXCTL,IXGBE_HLREG0_CRCSTRIP);

    //broadcastとjamboフレームのレジスタセット
    //broadcastのセット FCTRL->filter control register(receive register)
    //BAM -> broadcast accept mode
    set_flag32(ix_dev->addr,IXGBE_FCTRL,IXGBE_FCTRL_BAM);
    //jamboフレーム5000?
    //set_reg32(ixgbe->dev,IXGBE_MAXFRS,5000);

    printf("end: init_tx_reg");
}

void init_rx_queue(struct ixgbe_device *ix_dev)
{
    printf("start: init_rx_queue");
    //ring bufferの確保と初期化
    //それに伴うレジスタの初期化
    //それぞれのdescriptorは16bitのサイズらしい
    //receive descriptor listのメモリ割り当て
    uint16_t i;
    for(i=0;i < ix_dev->num_rx_queues;i++){
        //advanced rx descriptorを有効化する
        set_reg32(ix_dev->addr,IXGBE_SRRCTL(i),(get_reg32(ix_dev->addr,IXGBE_SRRCTL(i)) & ~IXGBE_SRRCTL_DESCTYPE_MASK) | IXGBE_SRRCTL_DESCTYPE_ADV_ONEBUF);

        //でかいパケットからのoverflowing対策らしい
        set_flag32(ix_dev->addr,IXGBE_SRRCTL(i), IXGBE_SRRCTL_DROP_EN);

        //bufferを割り当てるには、キューのサイズ(descriptor単体)とその個数でかける
        uint32_t ring_size = sizeof(union ixgbe_adv_rx_desc) * NUM_RX_QUEUES;
        struct dma_address dma_addr = allocate_dma_addr(ring_size);
        
        //DMAアクティベーション初期の時、メモリにアクセスされることを防ぐために仮想アドレスを初期化?
        memset(dma_addr.virt_addr,-1,ring_size);

        //baseアドレスの計算
        set_reg32(ix_dev->addr,IXGBE_RDBAL(i),(uint32_t)(0xFFFFFFFFull & dma_addr.phy_addr));
        set_reg32(ix_dev->addr,IXGBE_RDBAH(i),(uint32_t)(dma_addr.phy_addr >> 32);

        //長さの計算
        set_reg32(ix_dev->addr,IXGBE_RDLEN(i),ring_size);

        //headもtailも先頭
        set_reg32(ix_dev->addr,IXGBE_RDH(i),0);
        set_reg32(ix_dev->addr,IXGBE_RDT(i),0);
    }

    //rxをenableする前にやっておくこと
    set_flag32(ix_dev->addr,IXGBE_CTRL_EXT,IXGBE_CTRL_EXT_NS_DIS);
    for(uint16_t i=0;i< ix_dev->num_rx_queue;i++){
            //DCA -> direct cache access
            clear_flag32(ix_dev->addr,IXGBE_DCA_RXCTRL(i), 0<<12);
    }
    printf("end: init_rx_queue");
}

void init_rx(struct ixgbe_device *ix_dev)
{
    printf("start: init_tx");
    //receiveを止める
    unset_flag32(ix_dev->addr,IXGBE_RXCTRL,IXGBE_RXCTRL_RXEN);
    //初期化処理
    //receive address registerはEEPROMのなかにあるから初期化処理もうされてる？
    init_rx_reg(ix_dev);
    
    //memoryとかdescriptorとかの初期化というか準備・allocation
    init_rx_queue(ix_dev);
    //receive再開
    set_flag32(ix_dev->addr,IXGBE_RXCTRL,IXGBE_RXCTRL_RXEN);
    printf("end: init_tx");
}

void init_tx_reg(struct ixgbe_device *ix_dev)
{
    printf("start: init_tx_reg");
    //IXGBE_HLREG0に設定したい内容のフラグを立てるためにorをとって立てる
    //今回だとcrcオフロードとパケットパディング 
    set_flag32(ix_dev->addr,IXGBE_HLREG0,IXGBE_HLREG0_TXCRCEN | IXGBE_HLREG0_TXPADEN);
    //tcpのセグメンテーション設定とか言われてるけどまだ喋れないのでパス
    //毎度毎度のpage size初期化
    int i=0;
    for(i=0;i<8;i++){
        set_flag32(ix_dev->addr,IXGBE_TXPBSIZE(i),0);
    }
    //DCB->data center bridging(データセンターのトランスポートのロスレスのイーサネット拡張らしい
    //つまりいらない？
    //DCBとVT(virtualization)なし
    set_reg32(ix_dev->addr,IXGBE_DTMXSZRQ,0xFFFF);
    //0にしろと書かれていたので
    unset_flag32(ix_dev->addr,IXGBE_RTTDCS,IXGBE_RTTDCS_ARBDIS);
    printf("end: init_tx_reg");
}

void init_tx_queue(struct ixgbe_device *ix_dev)
{
    printf("start: init_tx_queue");
    uint16_t i;
    for(i=0;i<ix_dev->num_tx_queues;i++){
        //キューの割り当て
        //レジスタの初期化
        uint32_t tx_ring = sizeof(union ixgbe_adv_tx_desc)*NUM_TX_QUEUES;
        struct dma_address dma_addr = allocate_dma_address(tx_ring);
        
        //rxの時と同様例のテクニック
        memset(dma_addr.virt_addr,-1,tx_ring);
        
        //baseアドレスの設定
        set_reg32(ix_dev->addr,IXGBE_TDBAL(i),(uint32_t)(dma_addr.phy_addr & 0xFFFFFFFFul));
        set_reg32(ix_dev->addr,IXGBE_TDBAH(i),(uint32_t)(dma_addr.phy_addr >> 32));

        //長さセット
        set_reg32(ix_dev->addr,IXGBE_TDLEN(i),tx_ring);
        set_flag32(ix_dev->addr,IXGBE_TXDCTL(i),IXGBE_TXDCRL_ENABLE);

        //高い性能とpcieの低レイテンシを得るためのtransmit control descriptorのmagic
        //dpdkで使われてるっぽい
        //transmition latencyを抑えるには、PTHRETHはできるだけ高くして、HTHRETHとWTHRESHはできるだけ低くするっぽい
        //pcieのオーバーヘッドを小さくするには、PtHRETHはできるだけ小さく、HTHRETHとWTHRESHはできるだけ大きくらしい
        uint32_t txdctl = get_reg32(ix_dev->addr,IXGBE_TXDCTL(i));
        txdctl &= ~(0x3F | (0x3F << 8) | (0x3F << 16));
        txdctl |= (36 | (8<<8) | (4<<16));
        set_reg32(ix_dev->addr,IXGBE_TXDCTL(i),txdctl);
    }
    printf("end: init_tx_queue");
}

void init_tx(struct ixgbe_device *ix_dev)
{
    printf("start: init_tx")
    init_tx_reg(ix_dev);
    init_tx_queue(ix_dev);
    //enable dma
    set_reg32(ix_dev->addr,IXGBE_DMATXCTL,IXGBE_DMATXCTL_TE);
    printf("end: init_tx");
}

void start_rx_queue(struct ixgbe_device *ix_dev,uint16_t i)
{
    printf("start rx queue %d",i);

    struct rx_queue *rxq = ((struct rx_queue*)(ix_dev->rx_queues)) + i;
    int mempool_size = NUM_RX_QUEUE_ENTRIES + NUM_TX_QUEUE_ENTRIES;

    rxq->mempool = alloc_mem_for_mempool(mempool_size,PKT_BUF_ENTRY_SIZE);

    for(int i=0;i<rxq->num_entries;i++){
            volatile union ixgbe_adv_rx_desc *rxd = rxq->descriptors + i;
            struct pkt_buf *buf = pkt_buf_alloc(rxq->mempool);
            if(!buf){
                  error("failed to allocate buf");
            }
            //offsetof(型,メンバ指示子); メンバ指示子までのオフセット得られる
            rxd->read.pkt_addr = buf->buf_addr_phy + offsetof(struct pkt_buf,data);
            rxd->read.hdr_addr = 0;
            rxq->virtual_address[i] = buf;
    }
    //rx queueをenable
    set_flags32(ix_dev->addr,IXGBE_RXDCTL(i),IXGBE_RXDCTL_ENABLE);
    wait_enable_reset(ix_dev->addr,IXGBE_RXDCTL(i),IXGBE_RXDCTL_ENABLE);
    //もう一回初期化？？
    set_reg32(ix_dev->addr,IXGBE_RDH(i),0);
    set_reg32(ix_:dev->addr,IXGBE_RDT(i),rxq->num_entries - 1);
    
    printf("start rx queue %d",i);
}

void start_tx_queue(struct ixgbe_device *ix_dev,int i)
{
        printf("start: start_tx_queue %d",i);
        struct tx_queue *txq = ((struct tx_queue*)(ix_dev->tx_queue)) + i;

        set_reg32(ix_dev->addr,IXGBE_TDH(i),0);
        set_reg32(ix_dev->addr,IXGBE_TDT(i),txq->num_entries - 1);

        set_flag32(ix_dev->addr,IXGBE_TXDCTL(i),IXGBE_TXDCTL_ENABLE);
        wait_enable_reset(ix_dev->addr,IXGBE_TXDCTL(i),IXGBE_TXDCTL_ENABLE);
        printf("end: start_tx_queue %d",i);
}

uint32_t ixgbe_get_link_speed(struct *ixgbe_device ix_dev)
{
        printf("start: ixgbe_get_link_speed");
        uint32_t link_speed = get_reg32(ix_dev->addr,IXGBE_LINKS);
        if(!(link_speed & IXGBE_LINKS_UP)){
                return 0;
        }
        switch(link_speed & IXGBE_LINKS_SPEED_82599){
                case IXGBE_LINKS_SPEED_100_82599:
                    return 100;
                case IXGBE_LINKS_SPEED_1G_82599:
                    return 1000;
                case IXGBE_LINKS_SPEED_10G_82599:
                    return 10000;
                default:
                    return 0;
        }
        printf("end: ixgbe_get_link_speed");
}

void wait_for_link(struct ixgbe_device *ix_dev)
{
        printf("start: wait_for_link");
        printf("wait for link ...");
        uint32_t max_wait = 10000000;
        uint32_t poll_int = 100000;
        uint32_t speed;
        
        while(!(speed = ixgbe_get_link_speed(&ix_dev)) && max_wait > 0){
                usleep(poll_int);
                max_wait -= poll_int;
        }
        printf("Link speed is %d Mbit/s",speed);
        printf("end: wait_for_link");
}

/*initialization sequence*/
void do_init_seq(struct ixgbe_device *ix_dev)
{
    printf("start: do_init_seq");

    //EIMC registerに書き込めば、割り込み禁止
    set_reg32(ix_dev->addr,IXGBE_EIMC,0x7FFFFFFFF);

    //デバイスの初期化,初期化の後は10msec以上待つ必要があるぽい
    set_reg32(ix_dev->addr,IXGBE_CTRL,IXGBE_CTRL_RST_MASK);
    wait_enable_reset(ix_dev->addr,IXGBE_CTRL,IXGBE_CTRL_RST_MASK);
    //usleep(10000);

    //もう一回割り込み禁止しないとダメっぽい
    set_reg32(ix_dev->addr,IXGBE_EIMC,0x7FFFFFFFF);

    //Genaral Configurationレジスタの設定
    //EEPROMがauto completionされるのを待つっぽい(EEPROM->store product configuration information)
    wait_set_reg32(ix_dev->addr,IXGBE_EEC,IXGBE_EEC_ARD);

    //DMAの初期化ができるのを待つ
    wait_set_reg32(ix_dev->addr,IXGBE_RDRXCTL,IXGBE_RDRXCTL,IXGBE_RDRXCTL_DMAIDONE);
    
    //PHYとLINKの10G用の設定
    init_link(ix_dev);

    //Statisticsの初期化
    init_stats(ix_dev);

    //receiveの初期化
    init_rx(ix_dev);

    //transmitの初期化
    init_tx(ix_dev);

    uint16_t i;
    //rx and tx queueの初期化
    for(i=0;i<ix_dev->num_rx_queues;i++){
        start_rx_queue(ix_dev,i);
    }

    for(i=0;i<ix_dev->num_tx_queues;i++){
        start_tx_queue(ix_dev,i);
    }

    //割り込みの許可
    set_flags(ix_dev->addr,IXGBE_FCTRL,IXGBE_FCTRL_MPE | IXGBE_FCTRL_UPE);  

    //リンク待ち
    wait_for_link(ix_dev);

    printf("end: do_init_sequence");
}

struct ixgbe_device start_ixgbe(const char *pci_addr,uint16_t rx_queues,uint16_t tx_queues)
{
   if(getuid()){
       warn("Not running as root,this will probably fail");
   }
   if(rx_queues > MAX_QUEUES){
       error("Rx queues %d exceed MAX_QUEUES",rx_queues);
   }
   if(tx_queues > MAX_QUEUES){
      error("Tx queues %d exceed MAX_QUEUES",tx_queues);
   }

   struct ixgbe_device ix_dev;
   memset(ix_dev,0,sizeof(struct ixgbe_device));

   //strdup()->文字列をコピーして返す
   ix_dev.pci_addr = strdup(pci_addr);

   char path[PATH_MAX];
   sprintf(path,PATH_MAX,"/sys/bus/pci/devices/%s/iommu_group",pci_addr);
   struct stat buf;
   ix_dev.vfio = stat(path,&buf) == 0;
   if(ix_dev.vfio){
           ix_dev.vfio_fd = init_vfio(pci_addr);
           if(ix_dev.vfio_fd < 0){
                   error("faled to get vfio_fd");
           }
   }
   ix_dev.driver_name = driver_name;
   ix_dev.num_rx_queues = rx_queues;
   ix_dev.num_tx_queues = tx_queues;
   ix_dev.rx_batch = ixgbe_rx_batch;
   ix_dev.tx_batch = ixgbe_tx_batch;
   ix_dev.read_stats = ixgbe_read_stats;
   ix_dev.set_promisc = ixgbe_set_promisc;
   ix_dev.get_link_speed = ixgbe_get_link_speed;

   if(ix_dev.vfio){
           ix_dev.addr = vfio_map_region(ix_dev.vfio_fd,VFIO_PCI_BAR0_REGION_INDEX);
   }
   else{
           printf("can't use vfio");
           return -1;
   }
    
    ix_dev.rx_queues = calloc(rx_queues,sizeof(struct ixgbe_rx_queue) + sizeof(void *) * MAX_RX_QUEUE_ENTRIES);
    ix_dev.tx_queues = calloc(tx_queues,sizeof(struct ixgbe_tx_queue) + sizeof(void *) * MAX_TX_QUEUE_ENTRIES);

    do_init_seq(&ix_dev);

    return ix_dev;
}
