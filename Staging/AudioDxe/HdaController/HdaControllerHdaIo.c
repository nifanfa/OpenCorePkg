/*
 * File: HdaControllerHdaIo.c
 *
 * Copyright (c) 2018 John Davis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "HdaController.h"
#include <IndustryStandard/HdaRegisters.h>

STATIC
UINT32
HdaControllerCopyToRing (
  IN HDA_STREAM  *HdaStream,
  IN UINT32      Position,
  IN CONST UINT8 *Source,
  IN UINT32      Length
  )
{
  UINT32  CurrentPosition;
  UINT32  CopyLength;

  CurrentPosition = Position % HDA_STREAM_BUF_SIZE;
  while (Length > 0) {
    CopyLength = HDA_STREAM_BUF_SIZE - CurrentPosition;
    if (CopyLength > Length) {
      CopyLength = Length;
    }

    CopyMem (HdaStream->BufferData + CurrentPosition, Source, CopyLength);
    Source          += CopyLength;
    Length          -= CopyLength;
    CurrentPosition += CopyLength;
    if (CurrentPosition == HDA_STREAM_BUF_SIZE) {
      CurrentPosition = 0;
    }
  }

  return CurrentPosition;
}

// HDA I/O Device Path GUID.
EFI_GUID  gEfiHdaIoDevicePathGuid = EFI_HDA_IO_DEVICE_PATH_GUID;

/**
  Retrieves this codec's address.

  @param[in]  This              A pointer to the HDA_IO_PROTOCOL instance.
  @param[out] CodecAddress      The codec's address.

  @retval EFI_SUCCESS           The codec's address was returned.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
**/
EFI_STATUS
EFIAPI
HdaControllerHdaIoGetAddress (
  IN  EFI_HDA_IO_PROTOCOL  *This,
  OUT UINT8                *CodecAddress
  )
{
  HDA_IO_PRIVATE_DATA  *HdaPrivateData;

  // If parameters are NULL, return error.
  if ((This == NULL) || (CodecAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // Get private data and codec address.
  HdaPrivateData = HDA_IO_PRIVATE_DATA_FROM_THIS (This);
  *CodecAddress  = HdaPrivateData->HdaCodecAddress;
  return EFI_SUCCESS;
}

/**
  Sends a single command to the codec.

  @param[in]  This              A pointer to the HDA_IO_PROTOCOL instance.
  @param[in]  Node              The destination node.
  @param[in]  Verb              The verb to send.
  @param[out] Response          The response received.

  @retval EFI_SUCCESS           The verb was sent successfully and a response received.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
**/
EFI_STATUS
EFIAPI
HdaControllerHdaIoSendCommand (
  IN  EFI_HDA_IO_PROTOCOL  *This,
  IN  UINT8                Node,
  IN  UINT32               Verb,
  OUT UINT32               *Response
  )
{
  // Create verb list with single item.
  EFI_HDA_IO_VERB_LIST  HdaCodecVerbList;

  HdaCodecVerbList.Count     = 1;
  HdaCodecVerbList.Verbs     = &Verb;
  HdaCodecVerbList.Responses = Response;

  // Call SendCommands().
  return HdaControllerHdaIoSendCommands (This, Node, &HdaCodecVerbList);
}

/**
  Sends a set of commands to the codec.

  @param[in] This               A pointer to the HDA_IO_PROTOCOL instance.
  @param[in] Node               The destination node.
  @param[in] Verbs              The verbs to send. Responses will be delievered in the same list.

  @retval EFI_SUCCESS           The verbs were sent successfully and all responses received.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
**/
EFI_STATUS
EFIAPI
HdaControllerHdaIoSendCommands (
  IN EFI_HDA_IO_PROTOCOL   *This,
  IN UINT8                 Node,
  IN EFI_HDA_IO_VERB_LIST  *Verbs
  )
{
  // Create variables.
  HDA_IO_PRIVATE_DATA  *HdaPrivateData;

  // If parameters are NULL, return error.
  if ((This == NULL) || (Verbs == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // Get private data and send commands.
  HdaPrivateData = HDA_IO_PRIVATE_DATA_FROM_THIS (This);
  return HdaControllerSendCommands (HdaPrivateData->HdaControllerDev, HdaPrivateData->HdaCodecAddress, Node, Verbs);
}

EFI_STATUS
EFIAPI
HdaControllerHdaIoSetupStream (
  IN  EFI_HDA_IO_PROTOCOL       *This,
  IN  EFI_HDA_IO_PROTOCOL_TYPE  Type,
  IN  UINT16                    Format,
  OUT UINT8                     *StreamId
  )
{
  // DEBUG((DEBUG_INFO, "HdaControllerHdaIoSetupStream(): start\n"));

  // Create variables.
  EFI_STATUS           Status;
  HDA_IO_PRIVATE_DATA  *HdaIoPrivateData;
  HDA_CONTROLLER_DEV   *HdaControllerDev;
  EFI_PCI_IO_PROTOCOL  *PciIo;

  // Stream.
  HDA_STREAM  *HdaStream;
  UINT16      HdaStreamFormat;
  UINT16      HdaStreamFormatReadback;
  UINT8       HdaStreamId;
  EFI_TPL     OldTpl = 0;

  // If a parameter is invalid, return error.
  if ((This == NULL) || (Type >= EfiHdaIoTypeMaximum) || (StreamId == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // Get private data.
  HdaIoPrivateData = HDA_IO_PRIVATE_DATA_FROM_THIS (This);
  HdaControllerDev = HdaIoPrivateData->HdaControllerDev;
  PciIo            = HdaControllerDev->PciIo;
  HdaStreamFormatReadback = 0;

  // Get stream.
  if (Type == EfiHdaIoTypeOutput) {
    HdaStream = HdaIoPrivateData->HdaOutputStream;
  } else {
    HdaStream = HdaIoPrivateData->HdaInputStream;
  }

  // Get current stream ID.
  if (!HdaControllerGetStreamId (HdaStream, &HdaStreamId)) {
    Status = EFI_INVALID_PARAMETER;
    goto DONE;
  }

  // Is a stream ID allocated already? If so that means the stream is already
  // set up and we'll need to tear it down first.
  if (HdaStreamId > 0) {
    Status = EFI_ALREADY_STARTED;
    goto DONE;
  }

  // Raise TPL so we can't be messed with.
  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  // Find and allocate stream ID.
  for (UINT8 i = HDA_STREAM_ID_MIN; i <= HDA_STREAM_ID_MAX; i++) {
    if (!(HdaControllerDev->StreamIdMapping & (1 << i))) {
      HdaControllerDev->StreamIdMapping |= (1 << i);
      HdaStreamId                        = i;
      break;
    }
  }

  // If stream ID is still zero, fail.
  if (HdaStreamId == 0) {
    Status = EFI_OUT_OF_RESOURCES;
    goto DONE;
  }

  // Get current format.
  Status = PciIo->Mem.Read (
                        PciIo,
                        EfiPciIoWidthUint16,
                        PCI_HDA_BAR,
                        HDA_REG_SDNFMT (HdaStream->Index),
                        1,
                        &HdaStreamFormat
                        );
  if (EFI_ERROR (Status)) {
    goto DONE;
  }

  // Reset stream if format has changed.
  if (Format != HdaStreamFormat) {
    // Reset stream.
    DEBUG ((DEBUG_VERBOSE, "HdaControllerHdaIoSetupStream(): format changed, resetting stream\n"));
    if (!HdaControllerResetStream (HdaStream)) {
      Status = EFI_INVALID_PARAMETER;
      goto DONE;
    }
  }

  // Set stream ID.
  if (!HdaControllerSetStreamId (HdaIoPrivateData->HdaOutputStream, HdaStreamId)) {
    Status = EFI_INVALID_PARAMETER;
    goto DONE;
  }

  *StreamId = HdaStreamId;

  // Set stream format.
  // DEBUG((DEBUG_INFO, "HdaControllerHdaIoSetupStream(): setting format 0x%X\n", Format));
  Status = PciIo->Mem.Write (
                        PciIo,
                        EfiPciIoWidthUint16,
                        PCI_HDA_BAR,
                        HDA_REG_SDNFMT (HdaStream->Index),
                        1,
                        &Format
                        );
  if (EFI_ERROR (Status)) {
    HdaControllerDiagnosticLog (
      HdaControllerDev,
      "setup stream=%u SDNFMT write=0x%04X read=0x%04X write_status=0x%08X\n",
      HdaStream->Index,
      Format,
      HdaStreamFormatReadback,
      (UINT32)Status
      );
    goto DONE;
  }

  Status = PciIo->Mem.Read (
                        PciIo,
                        EfiPciIoWidthUint16,
                        PCI_HDA_BAR,
                        HDA_REG_SDNFMT (HdaStream->Index),
                        1,
                        &HdaStreamFormatReadback
                        );
  HdaControllerDiagnosticLog (
    HdaControllerDev,
    "setup stream=%u SDNFMT write=0x%04X read=0x%04X read_status=0x%08X\n",
    HdaStream->Index,
    Format,
    HdaStreamFormatReadback,
    (UINT32)Status
    );
  if (EFI_ERROR (Status)) {
    goto DONE;
  }

  // Stream is ready.
  Status = EFI_SUCCESS;

DONE:
  // Restore TPL if needed.
  if (OldTpl) {
    gBS->RestoreTPL (OldTpl);
  }

  return Status;
}

EFI_STATUS
EFIAPI
HdaControllerHdaIoCloseStream (
  IN EFI_HDA_IO_PROTOCOL       *This,
  IN EFI_HDA_IO_PROTOCOL_TYPE  Type
  )
{
  // DEBUG((DEBUG_INFO, "HdaControllerHdaIoCloseStream(): start\n"));

  // Create variables.
  EFI_STATUS           Status;
  HDA_IO_PRIVATE_DATA  *HdaIoPrivateData;
  HDA_CONTROLLER_DEV   *HdaControllerDev;

  // Stream.
  HDA_STREAM  *HdaStream;
  UINT8       HdaStreamId;
  EFI_TPL     OldTpl = 0;

  // If a parameter is invalid, return error.
  if ((This == NULL) || (Type >= EfiHdaIoTypeMaximum)) {
    return EFI_INVALID_PARAMETER;
  }

  // Get private data.
  HdaIoPrivateData = HDA_IO_PRIVATE_DATA_FROM_THIS (This);
  HdaControllerDev = HdaIoPrivateData->HdaControllerDev;

  // Get stream.
  if (Type == EfiHdaIoTypeOutput) {
    HdaStream = HdaIoPrivateData->HdaOutputStream;
  } else {
    HdaStream = HdaIoPrivateData->HdaInputStream;
  }

  // Get current stream ID.
  if (!HdaControllerGetStreamId (HdaStream, &HdaStreamId)) {
    Status = EFI_INVALID_PARAMETER;
    goto DONE;
  }

  // Is a stream ID already at zero?
  if (HdaStreamId == 0) {
    Status = EFI_SUCCESS;
    goto DONE;
  }

  // Raise TPL so we can't be messed with (but beware nested gBS->SetTimer calls).
  OldTpl = gBS->RaiseTPL (TPL_DXE_CORE_TIMER);

  // Stop stream.
  Status = HdaControllerHdaIoStopStream (This, Type);
  if (EFI_ERROR (Status)) {
    goto DONE;
  }

  // Set stream ID to zero.
  if (!HdaControllerSetStreamId (HdaStream, 0)) {
    Status = EFI_INVALID_PARAMETER;
    goto DONE;
  }

  // De-allocate stream ID from bitmap.
  HdaControllerDev->StreamIdMapping &= ~(1 << HdaStreamId);

  // Stream closed successfully.
  Status = EFI_SUCCESS;

DONE:
  // Restore TPL if needed.
  if (OldTpl) {
    gBS->RestoreTPL (OldTpl);
  }

  return Status;
}

EFI_STATUS
EFIAPI
HdaControllerHdaIoGetStream (
  IN  EFI_HDA_IO_PROTOCOL       *This,
  IN  EFI_HDA_IO_PROTOCOL_TYPE  Type,
  OUT BOOLEAN                   *State
  )
{
  // DEBUG((DEBUG_INFO, "HdaControllerHdaIoGetStream(): start\n"));

  // Create variables.
  HDA_IO_PRIVATE_DATA  *HdaIoPrivateData;
  HDA_STREAM           *HdaStream;

  // If a parameter is invalid, return error.
  if ((This == NULL) || (Type >= EfiHdaIoTypeMaximum) || (State == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // Get private data.
  HdaIoPrivateData = HDA_IO_PRIVATE_DATA_FROM_THIS (This);

  // Get stream.
  if (Type == EfiHdaIoTypeOutput) {
    HdaStream = HdaIoPrivateData->HdaOutputStream;
  } else {
    HdaStream = HdaIoPrivateData->HdaInputStream;
  }

  // Get stream state.
  if (!HdaControllerGetStreamState (HdaStream, State)) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
HdaControllerHdaIoStartStream (
  IN EFI_HDA_IO_PROTOCOL         *This,
  IN EFI_HDA_IO_PROTOCOL_TYPE    Type,
  IN VOID                        *Buffer,
  IN UINTN                       BufferLength,
  IN UINTN                       BufferPosition OPTIONAL,
  IN EFI_HDA_IO_STREAM_CALLBACK  Callback OPTIONAL,
  IN VOID                        *Context1 OPTIONAL,
  IN VOID                        *Context2 OPTIONAL,
  IN VOID                        *Context3 OPTIONAL
  )
{
  DEBUG ((DEBUG_VERBOSE, "HdaControllerHdaIoStartStream(): start\n"));

  // Create variables.
  EFI_STATUS           Status;
  HDA_IO_PRIVATE_DATA  *HdaIoPrivateData;
  HDA_CONTROLLER_DEV   *HdaControllerDev;
  EFI_PCI_IO_PROTOCOL  *PciIo;

  // Stream.
  HDA_STREAM  *HdaStream;
  UINT8       HdaStreamId;
  UINT16      HdaStreamSts;
  UINT32      HdaStreamDmaPos;
  UINT32      HdaStreamLinkPos;
  UINT32      SourceLength;
  UINT32      CurrentPosition;
  UINT32      LinkAdvanced;

  // If a parameter is invalid, return error.
  if ((This == NULL) || (Type >= EfiHdaIoTypeMaximum) ||
      (Buffer == NULL) || (BufferLength == 0) || (BufferPosition >= BufferLength))
  {
    return EFI_INVALID_PARAMETER;
  }

  // Get private data.
  HdaIoPrivateData = HDA_IO_PRIVATE_DATA_FROM_THIS (This);
  HdaControllerDev = HdaIoPrivateData->HdaControllerDev;
  PciIo            = HdaControllerDev->PciIo;

  // Get stream.
  if (Type == EfiHdaIoTypeOutput) {
    HdaStream = HdaIoPrivateData->HdaOutputStream;
  } else {
    HdaStream = HdaIoPrivateData->HdaInputStream;
  }

  // Get current stream ID.
  if (!HdaControllerGetStreamId (HdaStream, &HdaStreamId)) {
    return EFI_INVALID_PARAMETER;
  }

  // Is a stream ID zero? If so that means the stream is not setup yet.
  if (HdaStreamId == 0) {
    return EFI_NOT_READY;
  }

  // Once an asynchronous transfer reaches its lead point, keep the HDA
  // engine running and append the next source directly to the DMA ring.
  if (HdaStream->StreamRunning) {
    if (HdaStream->BufferActive) {
      DEBUG ((
        DEBUG_WARN,
        "HDA: StartStream rejected while transfer active, length 0x%X\n",
        BufferLength
        ));
      return EFI_ALREADY_STARTED;
    }

    SourceLength = (UINT32)BufferLength - (UINT32)BufferPosition;
    if ((SourceLength == 0) ||
        (SourceLength > HDA_STREAM_BUF_SIZE - HdaStream->BufferQueuedLength))
    {
      return EFI_OUT_OF_RESOURCES;
    }

    Status = PciIo->Mem.Read (
                          PciIo,
                          EfiPciIoWidthFifoUint32,
                          PCI_HDA_BAR,
                          HDA_REG_SDNLPIB (HdaStream->Index),
                          1,
                          &HdaStreamLinkPos
                          );
    if (EFI_ERROR (Status)) {
      return EFI_INVALID_PARAMETER;
    }

    if (!HdaControllerGetStreamDmaPos (HdaStream, HdaStreamLinkPos, &HdaStreamDmaPos)) {
      return EFI_INVALID_PARAMETER;
    }

    if (HdaStreamLinkPos >= HdaStream->LinkPositionLast) {
      LinkAdvanced = HdaStreamLinkPos - HdaStream->LinkPositionLast;
    } else {
      LinkAdvanced = (HDA_STREAM_BUF_SIZE - HdaStream->LinkPositionLast) + HdaStreamLinkPos;
    }

    if (LinkAdvanced >= HdaStream->BufferQueuedLength) {
      HdaStream->BufferQueuedLength = 0;
    } else {
      HdaStream->BufferQueuedLength -= LinkAdvanced;
    }

    // The write tail is software-owned.  Do not reset it to LPIB when the
    // accounting cursor temporarily reaches zero: on physical controllers
    // LPIB may lag behind the DMA engine and would move the producer back into
    // data that has already been prefetched.  The poll path synchronises this
    // tail to LPIB only while the running stream is genuinely idle.
    CurrentPosition = HdaStream->BufferWritePosition;
    HdaStream->BufferWritePosition = CurrentPosition;
    HdaStream->BufferWritePosition = HdaControllerCopyToRing (
                                       HdaStream,
                                       CurrentPosition,
                                       (CONST UINT8 *)Buffer + BufferPosition,
                                       SourceLength
                                       );
    HdaStream->BufferQueuedLength += SourceLength;

    HdaStream->BufferSource         = Buffer;
    HdaStream->BufferSourceLength   = SourceLength;
    HdaStream->BufferSourcePosition = SourceLength;
    HdaStream->Callback             = Callback;
    HdaStream->CallbackContext1     = Context1;
    HdaStream->CallbackContext2     = Context2;
    HdaStream->CallbackContext3     = Context3;
    HdaStream->DmaPositionLast      = HdaStreamDmaPos;
    HdaStream->DmaPositionTotal     = 0;
    HdaStream->DiagnosticPollCount  = 0;
    HdaStream->LinkPositionLast     = HdaStreamLinkPos;
    HdaStream->LinkPositionTotal    = 0;
    HdaStream->BufferActive         = TRUE;
    HdaStream->BufferSilent         = FALSE;
    DEBUG ((
      DEBUG_INFO,
      "HDA: Append len 0x%X at 0x%X next 0x%X lpib 0x%X queued 0x%X\n",
      SourceLength,
      CurrentPosition,
      HdaStream->BufferWritePosition,
      HdaStreamLinkPos,
      HdaStream->BufferQueuedLength
      ));
    HdaControllerDiagnosticLog (
      HdaControllerDev,
      "append stream=%u length=0x%X write_start=0x%X write_end=0x%X LPIB=0x%08X DMA=0x%08X queued=0x%X\n",
      HdaStream->Index,
      SourceLength,
      CurrentPosition,
      HdaStream->BufferWritePosition,
      HdaStreamLinkPos,
      HdaStreamDmaPos,
      HdaStream->BufferQueuedLength
      );
    return EFI_SUCCESS;
  }

  // Reset completion bit.
  HdaStreamSts = HDA_REG_SDNSTS_BCIS;
  Status       = PciIo->Mem.Write (PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, HDA_REG_SDNSTS (HdaStream->Index), 1, &HdaStreamSts);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Read the single playback cursor used by both QEMU and physical HDA.
  Status = PciIo->Mem.Read (
                        PciIo,
                        EfiPciIoWidthFifoUint32,
                        PCI_HDA_BAR,
                        HDA_REG_SDNLPIB (HdaStream->Index),
                        1,
                        &HdaStreamLinkPos
                        );
  if (EFI_ERROR (Status)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!HdaControllerGetStreamDmaPos (HdaStream, HdaStreamLinkPos, &HdaStreamDmaPos)) {
    return EFI_INVALID_PARAMETER;
  }

  // These counters describe this finite transfer, not the previous one.
  // Keeping the old DmaPositionLast can make DmaPositionTotal jump over the
  // completion threshold immediately after a restart.
  HdaStream->DmaPositionLast       = HdaStreamDmaPos;
  HdaStream->DmaPositionTotal      = 0;
  HdaStream->DmaPositionChangedMax = 0;
  HdaStream->DiagnosticPollCount   = 0;
  HdaStream->DmaCheckCount         = 0;
  HdaStream->DmaCheckComplete      = FALSE;
  HdaStream->LinkPositionLast      = HdaStreamLinkPos;
  HdaStream->LinkPositionTotal     = 0;

  DEBUG ((
    DEBUG_INFO,
    "HDA: Stream %u DMA pos 0x%X\n",
    HdaStream->Index,
    HdaStreamDmaPos
    ));

  // Save pointer to buffer.
  HdaStream->BufferSource         = Buffer;
  HdaStream->BufferSourceLength   = (UINT32)BufferLength; // TODO: All APIs will transition to 32-bit lengths/offsets.
  HdaStream->BufferSourcePosition = (UINT32)BufferPosition;
  HdaStream->Callback             = Callback;
  HdaStream->CallbackContext1     = Context1;
  HdaStream->CallbackContext2     = Context2;
  HdaStream->CallbackContext3     = Context3;
  HdaStream->BufferQueuedLength   = (UINT32)BufferLength - (UINT32)BufferPosition;

  // Zero out buffer.
  ZeroMem (HdaStream->BufferData, HDA_STREAM_BUF_SIZE);

  // Copy the complete initial transfer into the DMA ring before starting the
  // engine.  Waiting for IOC polling to fill later BDL entries leaves zeros
  // at startup when the first timer tick is delayed.
  SourceLength = HdaStream->BufferSourceLength - HdaStream->BufferSourcePosition;
  if (SourceLength > HDA_STREAM_BUF_SIZE) {
    Status = EFI_OUT_OF_RESOURCES;
    goto STOP_STREAM;
  }
  HdaControllerCopyToRing (
    HdaStream,
    HdaStreamDmaPos,
    HdaStream->BufferSource + HdaStream->BufferSourcePosition,
    SourceLength
    );
  HdaStream->BufferSourcePosition = HdaStream->BufferSourceLength;

  HdaStream->BufferWritePosition =
    (HdaStreamDmaPos + HdaStream->BufferQueuedLength) % HDA_STREAM_BUF_SIZE;

  // Setup polling timer.
  HdaStream->BufferActive = TRUE;
  HdaStream->BufferSilent = FALSE;
  Status                  = gBS->SetTimer (HdaStream->PollTimer, TimerPeriodic, HDA_STREAM_POLL_TIME);
  if (EFI_ERROR (Status)) {
    goto STOP_STREAM;
  }

  // Change stream state.
  if (!HdaControllerSetStreamState (HdaStream, TRUE)) {
    Status = EFI_INVALID_PARAMETER;
    goto STOP_STREAM;
  }

  HdaStream->StreamRunning = TRUE;

  DEBUG ((
    DEBUG_INFO,
    "HDA: StartStream running buffer %p length 0x%X position 0x%X queued 0x%X\n",
    Buffer,
    BufferLength,
    BufferPosition,
    HdaStream->BufferQueuedLength
    ));

  HdaControllerDiagnosticLog (
    HdaControllerDev,
    "start stream=%u length=0x%X position=0x%X LPIB=0x%08X DMA=0x%08X queued=0x%X\n",
    HdaStream->Index,
    BufferLength,
    BufferPosition,
    HdaStreamLinkPos,
    HdaStreamDmaPos,
    HdaStream->BufferQueuedLength
    );
  return EFI_SUCCESS;

STOP_STREAM:
  // Stop stream.
  HdaControllerHdaIoStopStream (This, Type);
  return Status;
}

EFI_STATUS
EFIAPI
HdaControllerHdaIoStopStream (
  IN EFI_HDA_IO_PROTOCOL       *This,
  IN EFI_HDA_IO_PROTOCOL_TYPE  Type
  )
{
  // DEBUG((DEBUG_INFO, "HdaControllerHdaIoStopStream(): start\n"));

  // Create variables.
  EFI_STATUS           Status;
  HDA_IO_PRIVATE_DATA  *HdaIoPrivateData;

  // Stream.
  HDA_STREAM  *HdaStream;
  UINT8       HdaStreamId;

  // If a parameter is invalid, return error.
  if ((This == NULL) || (Type >= EfiHdaIoTypeMaximum)) {
    return EFI_INVALID_PARAMETER;
  }

  // Get private data.
  HdaIoPrivateData = HDA_IO_PRIVATE_DATA_FROM_THIS (This);

  // Get stream.
  if (Type == EfiHdaIoTypeOutput) {
    HdaStream = HdaIoPrivateData->HdaOutputStream;
  } else {
    HdaStream = HdaIoPrivateData->HdaInputStream;
  }

  // Get current stream ID.
  if (!HdaControllerGetStreamId (HdaStream, &HdaStreamId)) {
    return EFI_INVALID_PARAMETER;
  }

  // Is the stream ID zero? If so that means the stream is not setup yet.
  if (HdaStreamId == 0) {
    return EFI_NOT_READY;
  }

  // Cancel polling timer.
  Status = gBS->SetTimer (HdaStream->PollTimer, TimerCancel, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Stop stream.
  if (!HdaControllerSetStreamState (HdaStream, FALSE)) {
    return EFI_INVALID_PARAMETER;
  }

  HdaStream->StreamRunning       = FALSE;
  HdaStream->BufferQueuedLength  = 0;
  HdaStream->BufferWritePosition = 0;

  // Remove source buffer pointer.
  HdaStream->BufferActive         = FALSE;
  HdaStream->BufferSilent         = FALSE;
  HdaStream->BufferSource         = NULL;
  HdaStream->BufferSourceLength   = 0;
  HdaStream->BufferSourcePosition = 0;
  HdaStream->Callback             = NULL;
  HdaStream->CallbackContext1     = NULL;
  HdaStream->CallbackContext2     = NULL;
  HdaStream->CallbackContext3     = NULL;
  return EFI_SUCCESS;
}
