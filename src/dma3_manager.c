#include "global.h"
#include "dma3.h"

#define MAX_DMA_REQUESTS 128

#define DMA_REQUEST_COPY32 1
#define DMA_REQUEST_FILL32 2
#define DMA_REQUEST_COPY16 3
#define DMA_REQUEST_FILL16 4

struct Dma3Request
{
    const u8 *src;
    u8 *dest;
    u16 size;
    u16 mode;
    u32 value;
};

static struct Dma3Request sDma3Requests[MAX_DMA_REQUESTS];

static vbool8 sDma3ManagerLocked;
static u8 sDma3RequestCursor;

void ClearDma3Requests(void)
{
    int i;

    sDma3ManagerLocked = TRUE;
    sDma3RequestCursor = 0;

    for (i = 0; i < MAX_DMA_REQUESTS; i++)
    {
        sDma3Requests[i].size = 0;
        sDma3Requests[i].src = NULL;
        sDma3Requests[i].dest = NULL;
    }

    sDma3ManagerLocked = FALSE;
}

void ProcessDma3Requests(void)
{
    u16 bytesTransferred;

    if (sDma3ManagerLocked)
        return;

    bytesTransferred = 0;

    // as long as there are DMA requests to process (unless size or vblank is an issue), do not exit
    while (sDma3Requests[sDma3RequestCursor].size != 0)
    {
        bytesTransferred += sDma3Requests[sDma3RequestCursor].size;

        if (bytesTransferred > 40 * 1024)
            return; // don't transfer more than 40 KiB
        if (*(u8 *)REG_ADDR_VCOUNT > 224)
            return; // we're about to leave vblank, stop

        switch (sDma3Requests[sDma3RequestCursor].mode)
        {
        case DMA_REQUEST_COPY32: // regular 32-bit copy
            Dma3CopyLarge32_(sDma3Requests[sDma3RequestCursor].src,
                             sDma3Requests[sDma3RequestCursor].dest,
                             sDma3Requests[sDma3RequestCursor].size);
            break;
        case DMA_REQUEST_FILL32: // repeat a single 32-bit value across RAM
            Dma3FillLarge32_(sDma3Requests[sDma3RequestCursor].value,
                             sDma3Requests[sDma3RequestCursor].dest,
                             sDma3Requests[sDma3RequestCursor].size);
            break;
        case DMA_REQUEST_COPY16:    // regular 16-bit copy
            Dma3CopyLarge16_(sDma3Requests[sDma3RequestCursor].src,
                             sDma3Requests[sDma3RequestCursor].dest,
                             sDma3Requests[sDma3RequestCursor].size);
            break;
        case DMA_REQUEST_FILL16: // repeat a single 16-bit value across RAM
            Dma3FillLarge16_(sDma3Requests[sDma3RequestCursor].value,
                             sDma3Requests[sDma3RequestCursor].dest,
                             sDma3Requests[sDma3RequestCursor].size);
            break;
        }

        // Free the request
        sDma3Requests[sDma3RequestCursor].src = NULL;
        sDma3Requests[sDma3RequestCursor].dest = NULL;
        sDma3Requests[sDma3RequestCursor].size = 0;
        sDma3Requests[sDma3RequestCursor].mode = 0;
        sDma3Requests[sDma3RequestCursor].value = 0;
        sDma3RequestCursor++;

        if (sDma3RequestCursor >= MAX_DMA_REQUESTS) // loop back to the first DMA request
            sDma3RequestCursor = 0;
    }
}

#if WASM || RP2350
// These platforms have no DMA hardware and no interrupts. ProcessDma3Requests()
// only runs inside VBlankIntr(), which they call explicitly once per frame from
// WasmRunFrame() (src/main.c) rather than from an interrupt.
//
// That breaks every one of the game's `while (TRUE)` init loops that polls for
// a queued copy to land: the loop never returns to the frame loop, so the queue
// can never drain, so the poll never clears. It froze the party menu outright --
// AllocPartyMenuBgGfx() case 1 (src/party_menu.c) waits on
// IsDma3ManagerBusyWithBgCopy() from inside CB2_InitPartyMenu()'s `while (TRUE)`,
// taking the entire game with it, second screen included.
//
// With no hardware to wait on there is nothing to defer, so do the transfer
// immediately and never occupy a queue slot. Completion checks then report
// "done" on the first poll, and ProcessDma3Requests() runs over a permanently
// empty queue.
s16 RequestDma3Copy(const void *src, void *dest, u16 size, u8 mode)
{
    if (mode == 1)
    {
        Dma3CopyLarge32_(src, dest, size);
    }
    else
    {
        Dma3CopyLarge16_(src, dest, size);
    }

    // Slot 0 is never occupied here, so CheckForSpaceForDma3Request(0) reads
    // as complete -- which is exactly true, the copy already happened.
    return 0;
}

s16 RequestDma3Fill(s32 value, void *dest, u16 size, u8 mode)
{
    if (mode == 1)
    {
        Dma3FillLarge32_(value, dest, size);
    }
    else
    {
        Dma3FillLarge16_(value, dest, size);
    }

    return 0;
}
#else
s16 RequestDma3Copy(const void *src, void *dest, u16 size, u8 mode)
{
    int cursor;
    int i = 0;

    sDma3ManagerLocked = TRUE;
    cursor = sDma3RequestCursor;

    while (i < MAX_DMA_REQUESTS)
    {
        if (sDma3Requests[cursor].size == 0) // an empty request was found.
        {
            sDma3Requests[cursor].src = src;
            sDma3Requests[cursor].dest = dest;
            sDma3Requests[cursor].size = size;

            if (mode == 1)
                sDma3Requests[cursor].mode = DMA_REQUEST_COPY32;
            else
                sDma3Requests[cursor].mode = DMA_REQUEST_COPY16;

            sDma3ManagerLocked = FALSE;
            return cursor;
        }
        if (++cursor >= MAX_DMA_REQUESTS) // loop back to start.
            cursor = 0;
        i++;
    }
    sDma3ManagerLocked = FALSE;
    return -1;  // no free DMA request was found
}

s16 RequestDma3Fill(s32 value, void *dest, u16 size, u8 mode)
{
    int cursor;
    int i = 0;

    cursor = sDma3RequestCursor;
    sDma3ManagerLocked = TRUE;

    while (i < MAX_DMA_REQUESTS)
    {
        if (sDma3Requests[cursor].size == 0) // an empty request was found.
        {
            sDma3Requests[cursor].dest = dest;
            sDma3Requests[cursor].size = size;
            sDma3Requests[cursor].mode = mode;
            sDma3Requests[cursor].value = value;

            if(mode == 1)
                sDma3Requests[cursor].mode = DMA_REQUEST_FILL32;
            else
                sDma3Requests[cursor].mode = DMA_REQUEST_FILL16;

            sDma3ManagerLocked = FALSE;
            return cursor;
        }
        if (++cursor >= MAX_DMA_REQUESTS) // loop back to start.
            cursor = 0;
        i++;
    }
    sDma3ManagerLocked = FALSE;
    return -1;  // no free DMA request was found
}

#endif // WASM || RP2350

s16 CheckForSpaceForDma3Request(s16 index)
{
    int i = 0;

    if (index == -1)  // check if all requests are free
    {
        while (i < MAX_DMA_REQUESTS)
        {
            if (sDma3Requests[i].size != 0)
                return -1;
            i++;
        }
        return 0;
    }
    else  // check the specified request
    {
        if (sDma3Requests[index].size != 0)
            return -1;
        return 0;
    }
}
