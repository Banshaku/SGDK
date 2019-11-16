#include "dma.h"

#include "config.h"
#include "vdp.h"
#include "sys.h"
#include "memory.h"
#include "z80_ctrl.h"

#include "kdebug.h"
#include "tools.h"


//#define DMA_DEBUG

#define DMA_DEFAULT_QUEUE_SIZE      64

#define DMA_AUTOFLUSH               0x1
#define DMA_OVERCAPACITY_IGNORE     0x2


// we don't want to share it
extern vu32 VIntProcess;

// DMA queue
DMAOpInfo *dmaQueues = NULL;

// DMA queue settings
static u16 queueSize;
static u16 maxTransferPerFrame;
static u16 flag;

// current queue index (0 = empty / queueSize = full)
static u16 queueIndex;
static u16 queueIndexLimit;
static u32 queueTransferSize;

// do not share (assembly methods)
void flushQueue(u16 num);
void flushQueueSafe(u16 num, u16 z80restore);


void DMA_init()
{
    DMA_initEx(DMA_DEFAULT_QUEUE_SIZE, 0);
}

void DMA_initEx(u16 size, u16 capacity)
{
    if (size) queueSize = size;
    else queueSize = DMA_DEFAULT_QUEUE_SIZE;

    // 0 means no limit
    maxTransferPerFrame = capacity;

    // auto flush is enabled by default
    flag = DMA_AUTOFLUSH;

    // already allocated ?
    if (dmaQueues) MEM_free(dmaQueues);
    // allocate DMA queue
    dmaQueues = MEM_alloc(queueSize * sizeof(DMAOpInfo));

    // clear queue
    DMA_clearQueue();
}

bool DMA_getAutoFlush()
{
    return (flag & DMA_AUTOFLUSH)?TRUE:FALSE;
}

void DMA_setAutoFlush(bool value)
{
    if (value)
    {
        flag |= DMA_AUTOFLUSH;
        // auto flush enabled and transfer size > 0 --> set process on VBlank
        if (queueTransferSize > 0)
             VIntProcess |= PROCESS_DMA_TASK;
    }
    else flag &= ~DMA_AUTOFLUSH;
}

u16 DMA_getMaxTransferSize()
{
    return maxTransferPerFrame;
}

void DMA_setMaxTransferSize(u16 value)
{
    maxTransferPerFrame = value;
}

void DMA_setMaxTransferSizeToDefault()
{
    DMA_setMaxTransferSize(IS_PALSYSTEM?15000:7200);
}

bool DMA_getIgnoreOverCapacity()
{
    return (flag & DMA_OVERCAPACITY_IGNORE)?TRUE:FALSE;
}

void DMA_setIgnoreOverCapacity(bool value)
{
    if (value) flag |= DMA_OVERCAPACITY_IGNORE;
    else flag &= ~DMA_OVERCAPACITY_IGNORE;
}

void DMA_clearQueue()
{
    queueIndex = 0;
    queueIndexLimit = 0;
    queueTransferSize = 0;
}

void DMA_flushQueue()
{
    u16 i;
    u8 autoInc;

    // default
    i = queueIndex;

    // limit reached ?
    if (queueIndexLimit)
    {
        // we choose to ignore over capacity transfers ?
        if (flag & DMA_OVERCAPACITY_IGNORE)
        {
            i = queueIndexLimit;

#if (LIB_DEBUG != 0)
            KLog_U2_("DMA_flushQueue(..) warning: transfer size is above ", maxTransferPerFrame, " bytes (", queueTransferSize, "), some transfers are ignored.");
#endif
        }
#if (LIB_DEBUG != 0)
        else KLog_U2_("DMA_flushQueue(..) warning: transfer size is above ", maxTransferPerFrame, " bytes (", queueTransferSize, ").");
#endif
    }

#ifdef DMA_DEBUG
    KLog_U3("DMA_flushQueue: queueIndexLimit=", queueIndexLimit, " queueIndex=", queueIndex, " i=", i);
#endif

    // wait for DMA FILL / COPY operation to complete (otherwise we can corrupt VDP)
    VDP_waitDMACompletion();
    // save autoInc
    autoInc = VDP_getAutoInc();

#if (DMA_DISABLED != 0)
    // DMA disabled --> replace with software copy

    DMAOpInfo *info = dmaQueues;

    while(i--)
    {
        u16 len = (info->regLen & 0xFF) | ((info->regLen & 0xFF0000) >> 8);
        s16 step = info->regAddrMStep & 0xFF;
        u32 from = ((info->regAddrMStep & 0xFF0000) >> 7) | ((info->regAddrHAddrL & 0x7F00FF) << 1);
        // replace DMA command by WRITE command
        u32 cmd = info->regCtrlWrite & ~0x80;

        // software copy
        DMA_doSoftwareCopyDirect(cmd, from, len, step);

        // next
        info++;
    }
#else
    u16 z80restore;

    // define z80 BUSREQ restore state
    if (Z80_isBusTaken()) z80restore = 0x0100;
    else z80restore = 0x0000;

#if (HALT_Z80_ON_DMA != 0)
    vu16 *pw = (vu16*) Z80_HALT_PORT;

    // disable Z80 before processing DMA
    *pw = 0x0100;

    flushQueue(i);

    // re-enable Z80 after all DMA (safer method)
    *pw = z80restore;
#else
    flushQueueSafe(i, z80restore);
#endif

#endif  // DMA_DISABLED

    // can clear the queue now
    DMA_clearQueue();
    // restore autoInc
    VDP_setAutoInc(autoInc);
}

//static void flushQueue(u16 num)
//{
//    u32 *info = (u32*) dmaQueues;
//    vu32 *pl = (vu32*) GFX_CTRL_PORT;
//    u16 i = num;
//
//    // flush DMA queue
//    while(i--)
//    {
//        *pl = *info++;  // regLen = 0x94000x9300 | (len | (len << 8) & 0xFF00FF)
//        *pl = *info++;  // regAddrMStep = 0x96008F00 | ((from << 7) & 0xFF0000) | step
//        *pl = *info++;  // regAddrHAddrL = 0x97009500 | ((from >> 1) & 0x7F00FF)
//        *pl = *info++;  // regCtrlWrite =  GFX_DMA_xxx_ADDR(to)
//    }
//}
//
//static void flushQueueSafe(u16 num, u16 z80restore)
//{
//    // z80 BUSREQ off state
//    const u16 z80off = 0x0100;
//    const u16 z80on = z80restore;
//
//    u32 *info = (u32*) dmaQueues;
//    vu32 *pl = (vu32*) GFX_CTRL_PORT;
//    vu16 *pw = (vu16*) Z80_HALT_PORT;
//    u16 i = num;
//
//    // flush DMA queue
//    while(i--)
//    {
//        *pl = *info++;  // regLen = 0x94000x9300 | (len | (len << 8) & 0xFF00FF)
//        *pl = *info++;  // regAddrMStep = 0x96008F00 | ((from << 7) & 0xFF0000) | step
//        *pl = *info++;  // regAddrHAddrL = 0x97009500 | ((from >> 1) & 0x7F00FF)
//
//        // DISABLE and RE-ENABLE Z80 immediately
//        // This allow to avoid DMA failure on some MD
//        // when Z80 try to access 68k BUS at same time the DMA starts.
//        // BUS arbitrer lantecy will disable Z80 for a very small amont of time
//        // when DMA start, avoiding that situation to happen.
//        *pw = z80off;
//        *pw = z80on;
//
//        // then trigger DMA
//        *pl = *info++;  // regCtrlWrite =  GFX_DMA_xxx_ADDR(to)
//    }
//}

u16 DMA_getQueueSize()
{
    return queueIndex;
}

u32 DMA_getQueueTransferSize()
{
    return queueTransferSize;
}

u16 DMA_queueDma(u8 location, u32 from, u16 to, u16 len, u16 step)
{
    u32 newlen;
    u32 banklimitb;
    u32 banklimitw;
    DMAOpInfo *info;

    // queue is full --> error
    if (queueIndex >= queueSize)
    {
#if (LIB_DEBUG != 0)
        KDebug_Alert("DMA_queueDma(..) failed: queue is full !");
#endif

        // return FALSE as transfer will be ignored
        return FALSE;
    }

    // DMA works on 64 KW bank
    banklimitb = 0x20000 - (from & 0x1FFFF);
    banklimitw = banklimitb >> 1;
    // bank limit exceeded
    if (len > banklimitw)
    {
        // we first do the second bank transfer
        DMA_queueDma(location, from + banklimitb, to + banklimitb, len - banklimitw, step);
        newlen = banklimitw;
    }
    // ok, use normal len
    else newlen = len;

    // get DMA info structure and pass to next one
    info = &dmaQueues[queueIndex];

    // $14:len H  $13:len L (DMA length in word)
    info->regLen = ((newlen | (newlen << 8)) & 0xFF00FF) | 0x94009300;
    // $16:M  $f:step (DMA address M and Step register)
    info->regAddrMStep = (((from << 7) & 0xFF0000) | 0x96008F00) + step;
    // $17:H  $15:L (DMA address H & L)
    info->regAddrHAddrL = ((from >> 1) & 0x7F00FF) | 0x97009500;

    // Trigger DMA
    switch(location)
    {
        case DMA_VRAM:
            info->regCtrlWrite = GFX_DMA_VRAM_ADDR(to);
#ifdef DMA_DEBUG
            KLog_U4("DMA_queueDma: VRAM from=", from, " to=", to, " len=", len, " step=", step);
#endif
            break;

        case DMA_CRAM:
            info->regCtrlWrite = GFX_DMA_CRAM_ADDR(to);
#ifdef DMA_DEBUG
            KLog_U4("DMA_queueDma: CRAM from=", from, " to=", to, " len=", len, " step=", step);
#endif
            break;

        case DMA_VSRAM:
            info->regCtrlWrite = GFX_DMA_VSRAM_ADDR(to);
#ifdef DMA_DEBUG
            KLog_U4("DMA_queueDma: VSRAM from=", from, " to=", to, " len=", len, " step=", step);
#endif
            break;
    }

    // pass to next index
    queueIndex++;
    // keep trace of transfered size
    queueTransferSize += newlen << 1;

#ifdef DMA_DEBUG
    KLog_U2("  Queue index=", queueIndex, " new queueTransferSize=", queueTransferSize);
#endif

    // auto flush enabled --> set process on VBlank
    if (flag & DMA_AUTOFLUSH) VIntProcess |= PROCESS_DMA_TASK;

    // we are above the defined limit ?
    if (maxTransferPerFrame && (queueTransferSize > maxTransferPerFrame))
    {
        // first time we reach the limit ? store index where to stop transfer
        if (queueIndexLimit == 0)
        {
#if (LIB_DEBUG != 0)
            KLog_S3("DMA_queueDma(..) warning: transfer size limit raised on transfer #", queueIndex - 1, ", current size = ", queueTransferSize, "  max allowed = ", maxTransferPerFrame);
#endif

            // store limit index
            queueIndexLimit = queueIndex - 1;

#ifdef DMA_DEBUG
            KLog_U1("  Queue index limit set at ", queueIndexLimit);
#endif
        }

        // return FALSE if transfer will be ignored
        return (flag & DMA_OVERCAPACITY_IGNORE)?FALSE:TRUE;
    }

    return TRUE;
}

void DMA_waitCompletion()
{
    VDP_waitDMACompletion();
}

void DMA_doDma(u8 location, u32 from, u16 to, u16 len, s16 step)
{
#if (DMA_DISABLED != 0)
    // wait for DMA FILL / COPY operation to complete (otherwise we can corrupt VDP)
    VDP_waitDMACompletion();
    // DMA disabled --> replace with software copy
    DMA_doSoftwareCopy(location, from, to, len, step);
#else
    vu16 *pw;
    vu16 *pwz;
    u32 cmd;
    u32 newlen;
    u32 banklimitb;
    u32 banklimitw;
    u16 z80restore;

    // DMA works on 64 KW bank
    banklimitb = 0x20000 - (from & 0x1FFFF);
    banklimitw = banklimitb >> 1;
    // bank limit exceeded
    if (len > banklimitw)
    {
        // we first do the second bank transfer
        DMA_doDma(location, from + banklimitb, to + banklimitb, len - banklimitw, -1);
        newlen = banklimitw;
    }
    // ok, use normal len
    else newlen = len;

    if (step != -1)
        VDP_setAutoInc(step);

    // wait for DMA FILL / COPY operation to complete (otherwise we can corrupt VDP)
    VDP_waitDMACompletion();

    // define z80 BUSREQ restore state
    if (Z80_isBusTaken()) z80restore = 0x0100;
    else z80restore = 0x0000;

    pw = (vu16*) GFX_CTRL_PORT;

    // Setup DMA length (in word here)
    *pw = 0x9300 + (newlen & 0xff);
    *pw = 0x9400 + ((newlen >> 8) & 0xff);

    // Setup DMA address
    from >>= 1;
    *pw = 0x9500 + (from & 0xff);
    from >>= 8;
    *pw = 0x9600 + (from & 0xff);
    from >>= 8;
    *pw = 0x9700 + (from & 0x7f);

    switch(location)
    {
        default:
        case DMA_VRAM:
            cmd = GFX_DMA_VRAM_ADDR(to);
            break;

        case DMA_CRAM:
            cmd = GFX_DMA_CRAM_ADDR(to);
            break;

        case DMA_VSRAM:
            cmd = GFX_DMA_VSRAM_ADDR(to);
            break;
    }

    pwz = (vu16*) Z80_HALT_PORT;

    {
        vu32 cmdbuf[1];
        u16* cmdbufp;

        // force storing DMA command into memory
        cmdbuf[0] = cmd;

        // then force issuing DMA from memory word operand
        cmdbufp = (u16*) cmdbuf;
        // first command word
        *pw = *cmdbufp++;

        // DISABLE Z80
        *pwz = 0x0100;
#if (HALT_Z80_ON_DMA == 0)
        // RE-ENABLE it immediately before trigger DMA
        // We do that to avoid DMA failure on some MD
        // when Z80 try to access 68k BUS at same time the DMA starts.
        // BUS arbitrer lantecy will disable Z80 for a very small amont of time
        // when DMA start, avoiding that situation to happen !
        *pwz = z80restore;
#endif

        // trigger DMA (second word command wrote from memory to avoid possible failure on some MD)
        *pw = *cmdbufp;
    }

#if (HALT_Z80_ON_DMA != 0)
    // re-enable Z80 after DMA (safer method)
    *pwz = z80restore;
#endif
#endif  // DMA_DISABLED
}

void DMA_doVRamFill(u16 to, u16 len, u8 value, s16 step)
{
    vu16 *pw;
    vu32 *pl;
    u16 l;

    if (step != -1)
        VDP_setAutoInc(step);

    // need to do some adjustement because of the way VRAM fill is done
    if (len)
    {
        if (to & 1)
        {
            if (len < 3) l = 1;
            else l = len - 2;
        }
        else
        {
            if (len < 2) l = 1;
            else l = len - 1;
        }
    }
    // special value of 0, we don't care
    else l = len;

//    DMA_doVRamFill(0, 1, 0xFF, 1);    // 01
//    DMA_doVRamFill(0, 1, 0xFF, 1);    // 01-3
//    DMA_doVRamFill(0, 2, 0xFF, 1);    // 01-3
//    DMA_doVRamFill(0, 2, 0xFF, 1);    // 0123

    // wait for DMA FILL / COPY operation to complete
    VDP_waitDMACompletion();

    pw = (vu16*) GFX_CTRL_PORT;

    // Setup DMA length
    *pw = 0x9300 + (l & 0xFF);
    *pw = 0x9400 + ((l >> 8) & 0xFF);

    // Setup DMA operation (VRAM FILL)
    *pw = 0x9780;

    // Write VRam DMA destination address
    pl = (vu32*) GFX_CTRL_PORT;
    *pl = GFX_DMA_VRAM_ADDR(to);

    // set up value to fill (need to be 16 bits extended)
    pw = (vu16*) GFX_DATA_PORT;
    *pw = value | (value << 8);
}

void DMA_doVRamCopy(u16 from, u16 to, u16 len, s16 step)
{
    vu16 *pw;
    vu32 *pl;

    if (step != -1)
        VDP_setAutoInc(step);

    // wait for DMA FILL / COPY operation to complete
    VDP_waitDMACompletion();

    pw = (vu16*) GFX_CTRL_PORT;

    // Setup DMA length
    *pw = 0x9300 + (len & 0xff);
    *pw = 0x9400 + ((len >> 8) & 0xff);

    // Setup DMA address
    *pw = 0x9500 + (from & 0xff);
    *pw = 0x9600 + ((from >> 8) & 0xff);

    // Setup DMA operation (VRAM COPY)
    *pw = 0x97C0;

    // Write VRam DMA destination address (start DMA copy operation)
    pl = (vu32*) GFX_CTRL_PORT;
    *pl = GFX_DMA_VRAMCOPY_ADDR(to);
}

void DMA_doSoftwareCopy(u8 location, u32 from, u16 to, u16 len, s16 step)
{
    u32 cmd;

    switch(location)
    {
        default:
        case DMA_VRAM:
            cmd = GFX_WRITE_VRAM_ADDR(to);
            break;

        case DMA_CRAM:
            cmd = GFX_WRITE_CRAM_ADDR(to);
            break;

        case DMA_VSRAM:
            cmd = GFX_WRITE_VSRAM_ADDR(to);
            break;
    }

    DMA_doSoftwareCopyDirect(cmd, from, len, step);
}

void DMA_doSoftwareCopyDirect(u32 cmd, u32 from, u16 len, s16 step)
{
    vu16 *pw;
    vu32 *pl;
    u16 *src;
    u16 i;

    if (step != -1)
        VDP_setAutoInc(step);

    pl = (vu32*) GFX_CTRL_PORT;
    pw = (vu16*) GFX_DATA_PORT;
    src = (u16*) from;
    i = len;

    *pl = cmd;
    // do software copy (not optimized but we don't care as this is normally just for testing)
    while(i--) *pw = *src++;
}
