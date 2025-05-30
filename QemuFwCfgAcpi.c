/** @file
  OVMF ACPI support using QEMU's fw-cfg interface

  Copyright (c) 2008 - 2014, Intel Corporation. All rights reserved.<BR>
  Copyright (C) 2012-2014, Red Hat, Inc.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <IndustryStandard/Acpi.h>            // EFI_ACPI_DESCRIPTION_HEADER
#include <IndustryStandard/QemuLoader.h>      // QEMU_LOADER_FNAME_SIZE
#include <IndustryStandard/UefiTcgPlatform.h>
#include <Library/AcpiPlatformLib.h>
#include <Library/BaseLib.h>                  // AsciiStrCmp()
#include <Library/BaseMemoryLib.h>            // CopyMem()
#include <Library/DebugLib.h>                 // DEBUG()
#include <Library/MemoryAllocationLib.h>      // AllocatePool()
#include <Library/OrderedCollectionLib.h>     // OrderedCollectionMin()
#include <Library/QemuFwCfgLib.h>             // QemuFwCfgFindFile()
#include <Library/QemuFwCfgS3Lib.h>           // QemuFwCfgS3Enabled()
#include <Library/UefiBootServicesTableLib.h> // gBS
#include <Library/TpmMeasurementLib.h>
#include "vrom.h"
#include "vrom_table.h"

//
// The user structure for the ordered collection that will track the fw_cfg
// blobs under processing.
//
typedef struct {
  UINT8      File[QEMU_LOADER_FNAME_SIZE]; // NUL-terminated name of the fw_cfg
                                           // blob. This is the ordering / search
                                           // key.
  UINTN      Size;                         // The number of bytes in this blob.
  UINT8      *Base;                        // Pointer to the blob data.
  BOOLEAN    HostsOnlyTableData;           // TRUE iff the blob has been found to
                                           // only contain data that is directly
                                           // part of ACPI tables.
} BLOB;

/**
  Compare a standalone key against a user structure containing an embedded key.

  @param[in] StandaloneKey  Pointer to the bare key.

  @param[in] UserStruct     Pointer to the user structure with the embedded
                            key.

  @retval <0  If StandaloneKey compares less than UserStruct's key.

  @retval  0  If StandaloneKey compares equal to UserStruct's key.

  @retval >0  If StandaloneKey compares greater than UserStruct's key.
**/
STATIC
INTN
/**
 * @brief Compares a standalone ASCII key string to the file name in a BLOB structure.
 *
 * @param StandaloneKey Pointer to a NUL-terminated ASCII string key.
 * @param UserStruct Pointer to a BLOB structure containing the file name.
 * @return INTN Zero if the keys match, a nonzero value otherwise.
 */
EFIAPI
BlobKeyCompare (
  IN CONST VOID  *StandaloneKey,
  IN CONST VOID  *UserStruct
  )
{
  CONST BLOB  *Blob;

  Blob = UserStruct;
  return AsciiStrCmp (StandaloneKey, (CONST CHAR8 *)Blob->File);
}

/**
  Comparator function for two user structures.

  @param[in] UserStruct1  Pointer to the first user structure.

  @param[in] UserStruct2  Pointer to the second user structure.

  @retval <0  If UserStruct1 compares less than UserStruct2.

  @retval  0  If UserStruct1 compares equal to UserStruct2.

  @retval >0  If UserStruct1 compares greater than UserStruct2.
**/
STATIC
INTN
/**
 * @brief Compares two BLOB structures by their file name keys.
 *
 * This function compares the file name of the first BLOB structure to the file name of the second BLOB structure using ASCII string comparison.
 *
 * @param UserStruct1 Pointer to the first BLOB structure.
 * @param UserStruct2 Pointer to the second BLOB structure.
 * @return INTN Negative if UserStruct1's file name is less, zero if equal, positive if greater.
 */
EFIAPI
BlobCompare (
  IN CONST VOID  *UserStruct1,
  IN CONST VOID  *UserStruct2
  )
{
  CONST BLOB  *Blob1;

  Blob1 = UserStruct1;
  return BlobKeyCompare (Blob1->File, UserStruct2);
}

/**
  Comparator function for two opaque pointers, ordering on (unsigned) pointer
  value itself.
  Can be used as both Key and UserStruct comparator.

  @param[in] Pointer1  First pointer.

  @param[in] Pointer2  Second pointer.

  @retval <0  If Pointer1 compares less than Pointer2.

  @retval  0  If Pointer1 compares equal to Pointer2.

  @retval >0  If Pointer1 compares greater than Pointer2.
**/
STATIC
INTN
/**
 * @brief Compares two pointers by their unsigned address values.
 *
 * @param Pointer1 First pointer to compare.
 * @param Pointer2 Second pointer to compare.
 * @return int Returns 0 if the pointers are equal, -1 if Pointer1 is less than Pointer2, or 1 if Pointer1 is greater than Pointer2.
 */
EFIAPI
PointerCompare (
  IN CONST VOID  *Pointer1,
  IN CONST VOID  *Pointer2
  )
{
  if (Pointer1 == Pointer2) {
    return 0;
  }

  if ((UINTN)Pointer1 < (UINTN)Pointer2) {
    return -1;
  }

  return 1;
}

/**
  Comparator function for two ASCII strings. Can be used as both Key and
  UserStruct comparator.

  This function exists solely so we can avoid casting &AsciiStrCmp to
  ORDERED_COLLECTION_USER_COMPARE and ORDERED_COLLECTION_KEY_COMPARE.

  @param[in] AsciiString1  Pointer to the first ASCII string.

  @param[in] AsciiString2  Pointer to the second ASCII string.

  @return  The return value of AsciiStrCmp (AsciiString1, AsciiString2).
**/
STATIC
INTN
/**
 * @brief Compares two ASCII strings for equality.
 *
 * Performs a case-sensitive comparison of two NUL-terminated ASCII strings.
 *
 * @return Zero if the strings are identical; a positive or negative value indicating the difference between the first differing characters otherwise.
 */
EFIAPI
AsciiStringCompare (
  IN CONST VOID  *AsciiString1,
  IN CONST VOID  *AsciiString2
  )
{
  return AsciiStrCmp (AsciiString1, AsciiString2);
}

/**
 * @brief Releases all resources associated with a collection of 32-bit restricted allocations.
 *
 * Frees all entries and uninitializes the ORDERED_COLLECTION used to track blobs that must be allocated below 4GB.
 *
 * @param[in] AllocationsRestrictedTo32Bit The collection to release.
 */
STATIC
VOID
ReleaseAllocationsRestrictedTo32Bit (
  IN ORDERED_COLLECTION  *AllocationsRestrictedTo32Bit
  )
{
  ORDERED_COLLECTION_ENTRY  *Entry, *Entry2;

  for (Entry = OrderedCollectionMin (AllocationsRestrictedTo32Bit);
       Entry != NULL;
       Entry = Entry2)
  {
    Entry2 = OrderedCollectionNext (Entry);
    OrderedCollectionDelete (AllocationsRestrictedTo32Bit, Entry, NULL);
  }

  OrderedCollectionUninit (AllocationsRestrictedTo32Bit);
}

/**
 * @brief Collects names of blobs requiring 32-bit address allocation from the loader script.
 *
 * Iterates over the QEMU loader script entries and identifies blobs referenced by QEMU_LOADER_ADD_POINTER commands with a pointer size less than 8 bytes. These blobs must be allocated below 4GB to ensure pointer patching is valid. The function populates an ORDERED_COLLECTION with the names of such blobs.
 *
 * @param[out] AllocationsRestrictedTo32Bit Receives a collection of blob names restricted to 32-bit allocation.
 * @param[in] LoaderStart Pointer to the first entry in the loader script.
 * @param[in] LoaderEnd Pointer one past the last entry in the loader script.
 *
 * @retval EFI_SUCCESS The collection was populated successfully.
 * @retval EFI_OUT_OF_RESOURCES Memory allocation failed.
 * @retval EFI_PROTOCOL_ERROR The loader script contains malformed entries.
 */
STATIC
EFI_STATUS
CollectAllocationsRestrictedTo32Bit (
  OUT ORDERED_COLLECTION      **AllocationsRestrictedTo32Bit,
  IN CONST QEMU_LOADER_ENTRY  *LoaderStart,
  IN CONST QEMU_LOADER_ENTRY  *LoaderEnd
  )
{
  ORDERED_COLLECTION       *Collection;
  CONST QEMU_LOADER_ENTRY  *LoaderEntry;
  EFI_STATUS               Status;

  Collection = OrderedCollectionInit (AsciiStringCompare, AsciiStringCompare);
  if (Collection == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (LoaderEntry = LoaderStart; LoaderEntry < LoaderEnd; ++LoaderEntry) {
    CONST QEMU_LOADER_ADD_POINTER  *AddPointer;

    if (LoaderEntry->Type != QemuLoaderCmdAddPointer) {
      continue;
    }

    AddPointer = &LoaderEntry->Command.AddPointer;

    if (AddPointer->PointerSize >= 8) {
      continue;
    }

    if (AddPointer->PointeeFile[QEMU_LOADER_FNAME_SIZE - 1] != '\0') {
      DEBUG ((DEBUG_ERROR, "%a: malformed file name\n", __func__));
      Status = EFI_PROTOCOL_ERROR;
      goto RollBack;
    }

    Status = OrderedCollectionInsert (
               Collection,
               NULL,                           // Entry
               (VOID *)AddPointer->PointeeFile
               );
    switch (Status) {
      case EFI_SUCCESS:
        DEBUG ((
          DEBUG_VERBOSE,
          "%a: restricting blob \"%a\" from 64-bit allocation\n",
          __func__,
          AddPointer->PointeeFile
          ));
        break;
      case EFI_ALREADY_STARTED:
        //
        // The restriction has been recorded already.
        //
        break;
      case EFI_OUT_OF_RESOURCES:
        goto RollBack;
      default:
        ASSERT (FALSE);
    }
  }

  *AllocationsRestrictedTo32Bit = Collection;
  return EFI_SUCCESS;

RollBack:
  ReleaseAllocationsRestrictedTo32Bit (Collection);
  return Status;
}

/**
  Process a QEMU_LOADER_ALLOCATE command.

  @param[in] Allocate                      The QEMU_LOADER_ALLOCATE command to
                                           process.

  @param[in,out] Tracker                   The ORDERED_COLLECTION tracking the
                                           BLOB user structures created thus
                                           far.

  @param[in] AllocationsRestrictedTo32Bit  The ORDERED_COLLECTION populated by
                                           the function
                                           CollectAllocationsRestrictedTo32Bit,
                                           naming the fw_cfg blobs that must
                                           not be allocated from 64-bit address
                                           space.

  @retval EFI_SUCCESS           An area of whole AcpiNVS pages has been
                                allocated for the blob contents, and the
                                contents have been saved. A BLOB object (user
                                structure) has been allocated from pool memory,
                                referencing the blob contents. The BLOB user
                                structure has been linked into Tracker.

  @retval EFI_PROTOCOL_ERROR    Malformed fw_cfg file name has been found in
                                Allocate, or the Allocate command references a
                                file that is already known by Tracker.

  @retval EFI_UNSUPPORTED       Unsupported alignment request has been found in
                                Allocate.

  @retval EFI_OUT_OF_RESOURCES  Pool allocation failed.

  @return                       Error codes from QemuFwCfgFindFile() and
                                gBS->AllocatePages().
**/
STATIC
EFI_STATUS
/**
 * @brief Allocates memory for a fw_cfg blob and loads its data.
 *
 * Processes a QEMU_LOADER_ALLOCATE command by validating the file name and alignment, locating the corresponding fw_cfg file, allocating ACPI NVS memory (with 32-bit address restriction if required), reading the blob data from fw_cfg, and tracking the allocation. Returns an error if the input is malformed, the blob is duplicated, alignment is unsupported, or allocation fails.
 *
 * @param Allocate Pointer to the QEMU_LOADER_ALLOCATE command structure describing the allocation.
 * @param Tracker Collection tracking all allocated blobs.
 * @param AllocationsRestrictedTo32Bit Collection of blob names that must be allocated below 4GB.
 * @return EFI_STATUS EFI_SUCCESS on success, or error code on failure.
 */
EFIAPI
ProcessCmdAllocate (
  IN CONST QEMU_LOADER_ALLOCATE  *Allocate,
  IN OUT ORDERED_COLLECTION      *Tracker,
  IN ORDERED_COLLECTION          *AllocationsRestrictedTo32Bit
  )
{
  FIRMWARE_CONFIG_ITEM  FwCfgItem;
  UINTN                 FwCfgSize;
  EFI_STATUS            Status;
  UINTN                 NumPages;
  EFI_PHYSICAL_ADDRESS  Address;
  BLOB                  *Blob;

  if (Allocate->File[QEMU_LOADER_FNAME_SIZE - 1] != '\0') {
    DEBUG ((DEBUG_ERROR, "%a: malformed file name\n", __func__));
    return EFI_PROTOCOL_ERROR;
  }

  if (Allocate->Alignment > EFI_PAGE_SIZE) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: unsupported alignment 0x%x\n",
      __func__,
      Allocate->Alignment
      ));
    return EFI_UNSUPPORTED;
  }

  Status = QemuFwCfgFindFile ((CHAR8 *)Allocate->File, &FwCfgItem, &FwCfgSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: QemuFwCfgFindFile(\"%a\"): %r\n",
      __func__,
      Allocate->File,
      Status
      ));
    return Status;
  }

  NumPages = EFI_SIZE_TO_PAGES (FwCfgSize);
  Address  = MAX_UINT64;
  if (OrderedCollectionFind (
        AllocationsRestrictedTo32Bit,
        Allocate->File
        ) != NULL)
  {
    Address = MAX_UINT32;
  }

  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiACPIMemoryNVS,
                  NumPages,
                  &Address
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Blob = AllocatePool (sizeof *Blob);
  if (Blob == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreePages;
  }

  CopyMem (Blob->File, Allocate->File, QEMU_LOADER_FNAME_SIZE);
  Blob->Size               = FwCfgSize;
  Blob->Base               = (VOID *)(UINTN)Address;
  Blob->HostsOnlyTableData = TRUE;

  Status = OrderedCollectionInsert (Tracker, NULL, Blob);
  if (Status == RETURN_ALREADY_STARTED) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: duplicated file \"%a\"\n",
      __func__,
      Allocate->File
      ));
    Status = EFI_PROTOCOL_ERROR;
  }

  if (EFI_ERROR (Status)) {
    goto FreeBlob;
  }

  QemuFwCfgSelectItem (FwCfgItem);
  QemuFwCfgReadBytes (FwCfgSize, Blob->Base);
  ZeroMem (Blob->Base + Blob->Size, EFI_PAGES_TO_SIZE (NumPages) - Blob->Size);

  DEBUG ((
    DEBUG_VERBOSE,
    "%a: File=\"%a\" Alignment=0x%x Zone=%d Size=0x%Lx "
    "Address=0x%Lx\n",
    __func__,
    Allocate->File,
    Allocate->Alignment,
    Allocate->Zone,
    (UINT64)Blob->Size,
    (UINT64)(UINTN)Blob->Base
    ));

  //
  // Measure the data which is downloaded from QEMU.
  // It has to be done before it is consumed. Because the data will
  // be updated in the following operations.
  //
  TpmMeasureAndLogData (
    1,
    EV_PLATFORM_CONFIG_FLAGS,
    EV_POSTCODE_INFO_ACPI_DATA,
    ACPI_DATA_LEN,
    (VOID *)(UINTN)Blob->Base,
    Blob->Size
    );

  return EFI_SUCCESS;

FreeBlob:
  FreePool (Blob);

FreePages:
  gBS->FreePages (Address, NumPages);

  return Status;
}

/**
  Process a QEMU_LOADER_ADD_POINTER command.

  @param[in] AddPointer  The QEMU_LOADER_ADD_POINTER command to process.

  @param[in] Tracker     The ORDERED_COLLECTION tracking the BLOB user
                         structures created thus far.

  @retval EFI_PROTOCOL_ERROR  Malformed fw_cfg file name(s) have been found in
                              AddPointer, or the AddPointer command references
                              a file unknown to Tracker, or the pointer to
                              relocate has invalid location, size, or value, or
                              the relocated pointer value is not representable
                              in the given pointer size.

  @retval EFI_SUCCESS         The pointer field inside the pointer blob has
                              been relocated.
**/
STATIC
EFI_STATUS
/**
 * @brief Relocates a pointer field within a blob to reference another blob's memory.
 *
 * Processes a QEMU_LOADER_ADD_POINTER command by updating a pointer field in the specified blob to point to the absolute address of another blob, offset by the original pointer value. Validates file names, pointer sizes, offsets, and ensures the relocated pointer is representable. Returns an error if any validation fails.
 *
 * @param AddPointer The loader command describing the pointer relocation.
 * @param Tracker The collection of blobs referenced by file name.
 * @return EFI_STATUS EFI_SUCCESS on success, or EFI_PROTOCOL_ERROR on malformed input or invalid references.
 */
EFIAPI
ProcessCmdAddPointer (
  IN CONST QEMU_LOADER_ADD_POINTER  *AddPointer,
  IN CONST ORDERED_COLLECTION       *Tracker
  )
{
  ORDERED_COLLECTION_ENTRY  *TrackerEntry, *TrackerEntry2;
  BLOB                      *Blob, *Blob2;
  UINT8                     *PointerField;
  UINT64                    PointerValue;

  if ((AddPointer->PointerFile[QEMU_LOADER_FNAME_SIZE - 1] != '\0') ||
      (AddPointer->PointeeFile[QEMU_LOADER_FNAME_SIZE - 1] != '\0'))
  {
    DEBUG ((DEBUG_ERROR, "%a: malformed file name\n", __func__));
    return EFI_PROTOCOL_ERROR;
  }

  TrackerEntry  = OrderedCollectionFind (Tracker, AddPointer->PointerFile);
  TrackerEntry2 = OrderedCollectionFind (Tracker, AddPointer->PointeeFile);
  if ((TrackerEntry == NULL) || (TrackerEntry2 == NULL)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: invalid blob reference(s) \"%a\" / \"%a\"\n",
      __func__,
      AddPointer->PointerFile,
      AddPointer->PointeeFile
      ));
    return EFI_PROTOCOL_ERROR;
  }

  Blob  = OrderedCollectionUserStruct (TrackerEntry);
  Blob2 = OrderedCollectionUserStruct (TrackerEntry2);
  if (((AddPointer->PointerSize != 1) && (AddPointer->PointerSize != 2) &&
       (AddPointer->PointerSize != 4) && (AddPointer->PointerSize != 8)) ||
      (Blob->Size < AddPointer->PointerSize) ||
      (Blob->Size - AddPointer->PointerSize < AddPointer->PointerOffset))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: invalid pointer location or size in \"%a\"\n",
      __func__,
      AddPointer->PointerFile
      ));
    return EFI_PROTOCOL_ERROR;
  }

  PointerField = Blob->Base + AddPointer->PointerOffset;
  PointerValue = 0;
  CopyMem (&PointerValue, PointerField, AddPointer->PointerSize);
  if (PointerValue >= Blob2->Size) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: invalid pointer value in \"%a\"\n",
      __func__,
      AddPointer->PointerFile
      ));
    return EFI_PROTOCOL_ERROR;
  }

  //
  // The memory allocation system ensures that the address of the byte past the
  // last byte of any allocated object is expressible (no wraparound).
  //
  ASSERT ((UINTN)Blob2->Base <= MAX_ADDRESS - Blob2->Size);

  PointerValue += (UINT64)(UINTN)Blob2->Base;
  if ((AddPointer->PointerSize < 8) &&
      (RShiftU64 (PointerValue, AddPointer->PointerSize * 8) != 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: relocated pointer value unrepresentable in "
      "\"%a\"\n",
      __func__,
      AddPointer->PointerFile
      ));
    return EFI_PROTOCOL_ERROR;
  }

  CopyMem (PointerField, &PointerValue, AddPointer->PointerSize);

  DEBUG ((
    DEBUG_VERBOSE,
    "%a: PointerFile=\"%a\" PointeeFile=\"%a\" "
    "PointerOffset=0x%x PointerSize=%d\n",
    __func__,
    AddPointer->PointerFile,
    AddPointer->PointeeFile,
    AddPointer->PointerOffset,
    AddPointer->PointerSize
    ));
  return EFI_SUCCESS;
}

/**
  Process a QEMU_LOADER_ADD_CHECKSUM command.

  @param[in] AddChecksum  The QEMU_LOADER_ADD_CHECKSUM command to process.

  @param[in] Tracker      The ORDERED_COLLECTION tracking the BLOB user
                          structures created thus far.

  @retval EFI_PROTOCOL_ERROR  Malformed fw_cfg file name has been found in
                              AddChecksum, or the AddChecksum command
                              references a file unknown to Tracker, or the
                              range to checksum is invalid.

  @retval EFI_SUCCESS         The requested range has been checksummed.
**/
STATIC
EFI_STATUS
/**
 * @brief Computes and stores an 8-bit checksum over a specified range in a tracked blob.
 *
 * Validates the checksum command, locates the referenced blob, computes the checksum over the given range, and writes the result at the specified offset within the blob.
 *
 * @retval EFI_SUCCESS            The checksum was computed and stored successfully.
 * @retval EFI_PROTOCOL_ERROR     The command is malformed, references an invalid blob, or specifies an invalid range.
 */
EFIAPI
ProcessCmdAddChecksum (
  IN CONST QEMU_LOADER_ADD_CHECKSUM  *AddChecksum,
  IN CONST ORDERED_COLLECTION        *Tracker
  )
{
  ORDERED_COLLECTION_ENTRY  *TrackerEntry;
  BLOB                      *Blob;

  if (AddChecksum->File[QEMU_LOADER_FNAME_SIZE - 1] != '\0') {
    DEBUG ((DEBUG_ERROR, "%a: malformed file name\n", __func__));
    return EFI_PROTOCOL_ERROR;
  }

  TrackerEntry = OrderedCollectionFind (Tracker, AddChecksum->File);
  if (TrackerEntry == NULL) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: invalid blob reference \"%a\"\n",
      __func__,
      AddChecksum->File
      ));
    return EFI_PROTOCOL_ERROR;
  }

  Blob = OrderedCollectionUserStruct (TrackerEntry);
  if ((Blob->Size <= AddChecksum->ResultOffset) ||
      (Blob->Size < AddChecksum->Length) ||
      (Blob->Size - AddChecksum->Length < AddChecksum->Start))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: invalid checksum range in \"%a\"\n",
      __func__,
      AddChecksum->File
      ));
    return EFI_PROTOCOL_ERROR;
  }

  Blob->Base[AddChecksum->ResultOffset] = CalculateCheckSum8 (
                                            Blob->Base + AddChecksum->Start,
                                            AddChecksum->Length
                                            );
  DEBUG ((
    DEBUG_VERBOSE,
    "%a: File=\"%a\" ResultOffset=0x%x Start=0x%x "
    "Length=0x%x\n",
    __func__,
    AddChecksum->File,
    AddChecksum->ResultOffset,
    AddChecksum->Start,
    AddChecksum->Length
    ));
  return EFI_SUCCESS;
}

/**
 * @brief Processes a QEMU_LOADER_WRITE_POINTER command to update a pointer in a writable fw_cfg file.
 *
 * Validates the command, locates the target fw_cfg file and referenced blob, computes the absolute pointer value, and writes it into the specified offset in the fw_cfg file. If S3 resume is enabled, the pointer write is also recorded for replay after S3 resume. Marks the referenced blob as unreleasable after the pointer is written.
 *
 * @param[in] WritePointer   The QEMU_LOADER_WRITE_POINTER command to process.
 * @param[in] Tracker        The collection tracking BLOB structures created so far.
 * @param[in,out] S3Context  The S3_CONTEXT for capturing pointer writes for S3 resume, or NULL if S3 is disabled.
 *
 * @retval EFI_SUCCESS           The pointer was written successfully, and recorded for S3 resume if applicable.
 * @retval EFI_PROTOCOL_ERROR    The command is malformed, references unknown files or blobs, or specifies invalid pointer parameters.
 * @return                       Error codes from SaveCondensedWritePointerToS3Context() if S3 context recording fails.
 */
STATIC
EFI_STATUS
ProcessCmdWritePointer (
  IN     CONST QEMU_LOADER_WRITE_POINTER  *WritePointer,
  IN     CONST ORDERED_COLLECTION         *Tracker,
  IN OUT       S3_CONTEXT                 *S3Context OPTIONAL
  )
{
  RETURN_STATUS             Status;
  FIRMWARE_CONFIG_ITEM      PointerItem;
  UINTN                     PointerItemSize;
  ORDERED_COLLECTION_ENTRY  *PointeeEntry;
  BLOB                      *PointeeBlob;
  UINT64                    PointerValue;

  if ((WritePointer->PointerFile[QEMU_LOADER_FNAME_SIZE - 1] != '\0') ||
      (WritePointer->PointeeFile[QEMU_LOADER_FNAME_SIZE - 1] != '\0'))
  {
    DEBUG ((DEBUG_ERROR, "%a: malformed file name\n", __func__));
    return EFI_PROTOCOL_ERROR;
  }

  Status = QemuFwCfgFindFile (
             (CONST CHAR8 *)WritePointer->PointerFile,
             &PointerItem,
             &PointerItemSize
             );
  PointeeEntry = OrderedCollectionFind (Tracker, WritePointer->PointeeFile);
  if (RETURN_ERROR (Status) || (PointeeEntry == NULL)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: invalid fw_cfg file or blob reference \"%a\" / \"%a\"\n",
      __func__,
      WritePointer->PointerFile,
      WritePointer->PointeeFile
      ));
    return EFI_PROTOCOL_ERROR;
  }

  if (((WritePointer->PointerSize != 1) && (WritePointer->PointerSize != 2) &&
       (WritePointer->PointerSize != 4) && (WritePointer->PointerSize != 8)) ||
      (PointerItemSize < WritePointer->PointerSize) ||
      (PointerItemSize - WritePointer->PointerSize <
       WritePointer->PointerOffset))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: invalid pointer location or size in \"%a\"\n",
      __func__,
      WritePointer->PointerFile
      ));
    return EFI_PROTOCOL_ERROR;
  }

  PointeeBlob  = OrderedCollectionUserStruct (PointeeEntry);
  PointerValue = WritePointer->PointeeOffset;
  if (PointerValue >= PointeeBlob->Size) {
    DEBUG ((DEBUG_ERROR, "%a: invalid PointeeOffset\n", __func__));
    return EFI_PROTOCOL_ERROR;
  }

  //
  // The memory allocation system ensures that the address of the byte past the
  // last byte of any allocated object is expressible (no wraparound).
  //
  ASSERT ((UINTN)PointeeBlob->Base <= MAX_ADDRESS - PointeeBlob->Size);

  PointerValue += (UINT64)(UINTN)PointeeBlob->Base;
  if ((WritePointer->PointerSize < 8) &&
      (RShiftU64 (PointerValue, WritePointer->PointerSize * 8) != 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: pointer value unrepresentable in \"%a\"\n",
      __func__,
      WritePointer->PointerFile
      ));
    return EFI_PROTOCOL_ERROR;
  }

  //
  // If S3 is enabled, we have to capture the below fw_cfg actions in condensed
  // form, to be replayed during S3 resume.
  //
  if (S3Context != NULL) {
    EFI_STATUS  SaveStatus;

    SaveStatus = SaveCondensedWritePointerToS3Context (
                   S3Context,
                   (UINT16)PointerItem,
                   WritePointer->PointerSize,
                   WritePointer->PointerOffset,
                   PointerValue
                   );
    if (EFI_ERROR (SaveStatus)) {
      return SaveStatus;
    }
  }

  QemuFwCfgSelectItem (PointerItem);
  QemuFwCfgSkipBytes (WritePointer->PointerOffset);
  QemuFwCfgWriteBytes (WritePointer->PointerSize, &PointerValue);

  //
  // Because QEMU has now learned PointeeBlob->Base, we must mark PointeeBlob
  // as unreleasable, for the case when the whole linker/loader script is
  // handled successfully.
  //
  PointeeBlob->HostsOnlyTableData = FALSE;

  DEBUG ((
    DEBUG_VERBOSE,
    "%a: PointerFile=\"%a\" PointeeFile=\"%a\" "
    "PointerOffset=0x%x PointeeOffset=0x%x PointerSize=%d\n",
    __func__,
    WritePointer->PointerFile,
    WritePointer->PointeeFile,
    WritePointer->PointerOffset,
    WritePointer->PointeeOffset,
    WritePointer->PointerSize
    ));
  return EFI_SUCCESS;
}

/**
 * @brief Reverts a QEMU_LOADER_WRITE_POINTER command by zeroing the pointer field in the specified fw_cfg file.
 *
 * This function clears a previously written guest memory pointer in a fw_cfg file, effectively undoing the effect of a QEMU_LOADER_WRITE_POINTER command that was successfully processed earlier.
 *
 * @param[in] WritePointer Pointer to the QEMU_LOADER_WRITE_POINTER command structure describing the pointer to be zeroed.
 */
STATIC
VOID
UndoCmdWritePointer (
  IN CONST QEMU_LOADER_WRITE_POINTER  *WritePointer
  )
{
  RETURN_STATUS         Status;
  FIRMWARE_CONFIG_ITEM  PointerItem;
  UINTN                 PointerItemSize;
  UINT64                PointerValue;

  Status = QemuFwCfgFindFile (
             (CONST CHAR8 *)WritePointer->PointerFile,
             &PointerItem,
             &PointerItemSize
             );
  ASSERT_RETURN_ERROR (Status);

  PointerValue = 0;
  QemuFwCfgSelectItem (PointerItem);
  QemuFwCfgSkipBytes (WritePointer->PointerOffset);
  QemuFwCfgWriteBytes (WritePointer->PointerSize, &PointerValue);

  DEBUG ((
    DEBUG_VERBOSE,
    "%a: PointerFile=\"%a\" PointerOffset=0x%x PointerSize=%d\n",
    __func__,
    WritePointer->PointerFile,
    WritePointer->PointerOffset,
    WritePointer->PointerSize
    ));
}

//
// We'll be saving the keys of installed tables so that we can roll them back
// in case of failure. 128 tables should be enough for anyone (TM).
//
#define INSTALLED_TABLES_MAX  128

/**
  Process a QEMU_LOADER_ADD_POINTER command in order to see if its target byte
  array is an ACPI table, and if so, install it.

  This function assumes that the entire QEMU linker/loader command file has
  been processed successfully in a prior first pass.

  @param[in] AddPointer        The QEMU_LOADER_ADD_POINTER command to process.

  @param[in] Tracker           The ORDERED_COLLECTION tracking the BLOB user
                               structures.

  @param[in] AcpiProtocol      The ACPI table protocol used to install tables.

  @param[in,out] InstalledKey  On input, an array of INSTALLED_TABLES_MAX UINTN
                               elements, allocated by the caller. On output,
                               the function will have stored (appended) the
                               AcpiProtocol-internal key of the ACPI table that
                               the function has installed, if the AddPointer
                               command identified an ACPI table that is
                               different from RSDT and XSDT.

  @param[in,out] NumInstalled  On input, the number of entries already used in
                               InstalledKey; it must be in [0,
                               INSTALLED_TABLES_MAX] inclusive. On output, the
                               parameter is incremented if the AddPointer
                               command identified an ACPI table that is
                               different from RSDT and XSDT.

  @param[in,out] SeenPointers  The ORDERED_COLLECTION tracking the absolute
                               target addresses that have been pointed-to by
                               QEMU_LOADER_ADD_POINTER commands thus far. If a
                               target address is encountered for the first
                               time, and it identifies an ACPI table that is
                               different from RDST and XSDT, the table is
                               installed. If a target address is seen for the
                               second or later times, it is skipped without
                               taking any action.

  @retval EFI_INVALID_PARAMETER  NumInstalled was outside the allowed range on
                                 input.

  @retval EFI_OUT_OF_RESOURCES   The AddPointer command identified an ACPI
                                 table different from RSDT and XSDT, but there
                                 was no more room in InstalledKey.

  @retval EFI_SUCCESS            AddPointer has been processed. Either its
                                 absolute target address has been encountered
                                 before, or an ACPI table different from RSDT
                                 and XSDT has been installed (reflected by
                                 InstalledKey and NumInstalled), or RSDT or
                                 XSDT has been identified but not installed, or
                                 the fw_cfg blob pointed-into by AddPointer has
                                 been marked as hosting something else than
                                 just direct ACPI table contents.

  @return                        Error codes returned by
                                 AcpiProtocol->InstallAcpiTable().
**/
STATIC
EFI_STATUS
/**
 * @brief Processes a QEMU_LOADER_ADD_POINTER command in the second pass to identify and install ACPI tables.
 *
 * Examines the pointer target specified by the loader command to determine if it references a valid ACPI table or FACS structure. If a valid table is found (excluding RSDT and XSDT), installs it using the EFI_ACPI_TABLE_PROTOCOL and tracks the installation to prevent duplicates. Marks blobs as opaque if no ACPI table is found. Handles resource limits and avoids reprocessing already seen pointers.
 *
 * @param AddPointer The loader command describing the pointer relocation.
 * @param Tracker Collection of all tracked blobs.
 * @param AcpiProtocol ACPI table protocol for table installation.
 * @param InstalledKey Array for storing installed table keys.
 * @param NumInstalled Pointer to the count of installed tables; incremented on success.
 * @param SeenPointers Collection tracking already processed pointer values.
 * @return EFI_SUCCESS on success, or an appropriate EFI error code on failure.
 */
EFIAPI
Process2ndPassCmdAddPointer (
  IN     CONST QEMU_LOADER_ADD_POINTER  *AddPointer,
  IN     CONST ORDERED_COLLECTION       *Tracker,
  IN     EFI_ACPI_TABLE_PROTOCOL        *AcpiProtocol,
  IN OUT UINTN                          InstalledKey[INSTALLED_TABLES_MAX],
  IN OUT INT32                          *NumInstalled,
  IN OUT ORDERED_COLLECTION             *SeenPointers
  )
{
  CONST ORDERED_COLLECTION_ENTRY                      *TrackerEntry;
  CONST ORDERED_COLLECTION_ENTRY                      *TrackerEntry2;
  ORDERED_COLLECTION_ENTRY                            *SeenPointerEntry;
  CONST BLOB                                          *Blob;
  BLOB                                                *Blob2;
  CONST UINT8                                         *PointerField;
  UINT64                                              PointerValue;
  UINTN                                               Blob2Remaining;
  UINTN                                               TableSize;
  CONST EFI_ACPI_1_0_FIRMWARE_ACPI_CONTROL_STRUCTURE  *Facs;
  CONST EFI_ACPI_DESCRIPTION_HEADER                   *Header;
  EFI_STATUS                                          Status;

  if ((*NumInstalled < 0) || (*NumInstalled > INSTALLED_TABLES_MAX)) {
    return EFI_INVALID_PARAMETER;
  }

  TrackerEntry  = OrderedCollectionFind (Tracker, AddPointer->PointerFile);
  TrackerEntry2 = OrderedCollectionFind (Tracker, AddPointer->PointeeFile);
  Blob          = OrderedCollectionUserStruct (TrackerEntry);
  Blob2         = OrderedCollectionUserStruct (TrackerEntry2);
  PointerField  = Blob->Base + AddPointer->PointerOffset;
  PointerValue  = 0;
  CopyMem (&PointerValue, PointerField, AddPointer->PointerSize);

  //
  // We assert that PointerValue falls inside Blob2's contents. This is ensured
  // by the Blob2->Size check and later checks in ProcessCmdAddPointer().
  //
  Blob2Remaining = (UINTN)Blob2->Base;
  ASSERT (PointerValue >= Blob2Remaining);
  Blob2Remaining += Blob2->Size;
  ASSERT (PointerValue < Blob2Remaining);

  Status = OrderedCollectionInsert (
             SeenPointers,
             &SeenPointerEntry, // for reverting insertion in error case
             (VOID *)(UINTN)PointerValue
             );
  if (EFI_ERROR (Status)) {
    if (Status == RETURN_ALREADY_STARTED) {
      //
      // Already seen this pointer, don't try to process it again.
      //
      DEBUG ((
        DEBUG_VERBOSE,
        "%a: PointerValue=0x%Lx already processed, skipping.\n",
        __func__,
        PointerValue
        ));
      Status = EFI_SUCCESS;
    }

    return Status;
  }

  Blob2Remaining -= (UINTN)PointerValue;
  DEBUG ((
    DEBUG_VERBOSE,
    "%a: checking for ACPI header in \"%a\" at 0x%Lx "
    "(remaining: 0x%Lx): ",
    __func__,
    AddPointer->PointeeFile,
    PointerValue,
    (UINT64)Blob2Remaining
    ));

  TableSize = 0;

  //
  // To make our job simple, the FACS has a custom header. Sigh.
  //
  if (sizeof *Facs <= Blob2Remaining) {
    Facs = (EFI_ACPI_1_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(UINTN)PointerValue;

    if ((Facs->Length >= sizeof *Facs) &&
        (Facs->Length <= Blob2Remaining) &&
        (Facs->Signature ==
         EFI_ACPI_1_0_FIRMWARE_ACPI_CONTROL_STRUCTURE_SIGNATURE))
    {
      DEBUG ((
        DEBUG_VERBOSE,
        "found \"%-4.4a\" size 0x%x\n",
        (CONST CHAR8 *)&Facs->Signature,
        Facs->Length
        ));
      TableSize = Facs->Length;
    }
  }

  //
  // check for the uniform tables
  //
  if ((TableSize == 0) && (sizeof *Header <= Blob2Remaining)) {
    Header = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)PointerValue;

    if ((Header->Length >= sizeof *Header) &&
        (Header->Length <= Blob2Remaining) &&
        (CalculateSum8 ((CONST UINT8 *)Header, Header->Length) == 0))
    {
      //
      // This looks very much like an ACPI table from QEMU:
      // - Length field consistent with both ACPI and containing blob size
      // - checksum is correct
      //
      DEBUG ((
        DEBUG_VERBOSE,
        "found \"%-4.4a\" size 0x%x\n",
        (CONST CHAR8 *)&Header->Signature,
        Header->Length
        ));
      TableSize = Header->Length;

      //
      // Skip RSDT and XSDT because those are handled by
      // EFI_ACPI_TABLE_PROTOCOL automatically.
      if ((Header->Signature ==
           EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) ||
          (Header->Signature ==
           EFI_ACPI_2_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE))
      {
        return EFI_SUCCESS;
      }
    }
  }

  if (TableSize == 0) {
    DEBUG ((DEBUG_VERBOSE, "not found; marking fw_cfg blob as opaque\n"));
    Blob2->HostsOnlyTableData = FALSE;
    return EFI_SUCCESS;
  }

  if (*NumInstalled == INSTALLED_TABLES_MAX) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: can't install more than %d tables\n",
      __func__,
      INSTALLED_TABLES_MAX
      ));
    Status = EFI_OUT_OF_RESOURCES;
    goto RollbackSeenPointer;
  }

  Status = AcpiProtocol->InstallAcpiTable (
                           AcpiProtocol,
                           (VOID *)(UINTN)PointerValue,
                           TableSize,
                           &InstalledKey[*NumInstalled]
                           );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: InstallAcpiTable(): %r\n",
      __func__,
      Status
      ));
    goto RollbackSeenPointer;
  }

  ++*NumInstalled;
  return EFI_SUCCESS;

RollbackSeenPointer:
  OrderedCollectionDelete (SeenPointers, SeenPointerEntry, NULL);
  return Status;
}

/**
 * @brief Downloads, processes, and installs ACPI tables from QEMU firmware configuration.
 *
 * Retrieves the QEMU loader script from fw_cfg, parses and executes its commands to allocate memory, patch pointers, compute checksums, and install ACPI tables using the provided ACPI table protocol. Handles S3 resume support, error rollback, and injects an additional SSDT table with VROM data.
 *
 * @param[in] AcpiProtocol The ACPI table protocol used to install tables.
 *
 * @retval EFI_UNSUPPORTED        Firmware configuration is unavailable or an unsupported loader command is encountered.
 * @retval EFI_NOT_FOUND          Required fw_cfg files are missing.
 * @retval EFI_OUT_OF_RESOURCES   Memory allocation failed or too many tables found.
 * @retval EFI_PROTOCOL_ERROR     Invalid fw_cfg contents detected.
 * @return Status codes from AcpiProtocol->InstallAcpiTable().
 */
EFI_STATUS
EFIAPI
InstallQemuFwCfgTables (
  IN   EFI_ACPI_TABLE_PROTOCOL  *AcpiProtocol
  )
{
  EFI_STATUS                Status;
  FIRMWARE_CONFIG_ITEM      FwCfgItem;
  UINTN                     FwCfgSize;
  QEMU_LOADER_ENTRY         *LoaderStart;
  CONST QEMU_LOADER_ENTRY   *LoaderEntry, *LoaderEnd;
  CONST QEMU_LOADER_ENTRY   *WritePointerSubsetEnd;
  ORIGINAL_ATTRIBUTES       *OriginalPciAttributes;
  UINTN                     OriginalPciAttributesCount;
  ORDERED_COLLECTION        *AllocationsRestrictedTo32Bit;
  S3_CONTEXT                *S3Context;
  ORDERED_COLLECTION        *Tracker;
  UINTN                     *InstalledKey;
  INT32                     Installed;
  ORDERED_COLLECTION_ENTRY  *TrackerEntry, *TrackerEntry2;
  ORDERED_COLLECTION        *SeenPointers;
  ORDERED_COLLECTION_ENTRY  *SeenPointerEntry, *SeenPointerEntry2;
  EFI_HANDLE                QemuAcpiHandle;

  Status = QemuFwCfgFindFile ("etc/table-loader", &FwCfgItem, &FwCfgSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (FwCfgSize % sizeof *LoaderEntry != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: \"etc/table-loader\" has invalid size 0x%Lx\n",
      __func__,
      (UINT64)FwCfgSize
      ));
    return EFI_PROTOCOL_ERROR;
  }

  LoaderStart = AllocatePool (FwCfgSize);
  if (LoaderStart == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  EnablePciDecoding (&OriginalPciAttributes, &OriginalPciAttributesCount);
  QemuFwCfgSelectItem (FwCfgItem);
  QemuFwCfgReadBytes (FwCfgSize, LoaderStart);
  RestorePciDecoding (OriginalPciAttributes, OriginalPciAttributesCount);

  //
  // Measure the "etc/table-loader" which is downloaded from QEMU.
  // It has to be done before it is consumed. Because it would be
  // updated in the following operations.
  //
  TpmMeasureAndLogData (
    1,
    EV_PLATFORM_CONFIG_FLAGS,
    EV_POSTCODE_INFO_ACPI_DATA,
    ACPI_DATA_LEN,
    (VOID *)(UINTN)LoaderStart,
    FwCfgSize
    );

  LoaderEnd = LoaderStart + FwCfgSize / sizeof *LoaderEntry;

  AllocationsRestrictedTo32Bit = NULL;
  Status                       = CollectAllocationsRestrictedTo32Bit (
                                   &AllocationsRestrictedTo32Bit,
                                   LoaderStart,
                                   LoaderEnd
                                   );
  if (EFI_ERROR (Status)) {
    goto FreeLoader;
  }

  S3Context = NULL;
  if (QemuFwCfgS3Enabled ()) {
    //
    // Size the allocation pessimistically, assuming that all commands in the
    // script are QEMU_LOADER_WRITE_POINTER commands.
    //
    Status = AllocateS3Context (&S3Context, LoaderEnd - LoaderStart);
    if (EFI_ERROR (Status)) {
      goto FreeAllocationsRestrictedTo32Bit;
    }
  }

  Tracker = OrderedCollectionInit (BlobCompare, BlobKeyCompare);
  if (Tracker == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeS3Context;
  }

  //
  // first pass: process the commands
  //
  // "WritePointerSubsetEnd" points one past the last successful
  // QEMU_LOADER_WRITE_POINTER command. Now when we're about to start the first
  // pass, no such command has been encountered yet.
  //
  WritePointerSubsetEnd = LoaderStart;
  for (LoaderEntry = LoaderStart; LoaderEntry < LoaderEnd; ++LoaderEntry) {
    switch (LoaderEntry->Type) {
      case QemuLoaderCmdAllocate:
        Status = ProcessCmdAllocate (
                   &LoaderEntry->Command.Allocate,
                   Tracker,
                   AllocationsRestrictedTo32Bit
                   );
        break;

      case QemuLoaderCmdAddPointer:
        Status = ProcessCmdAddPointer (
                   &LoaderEntry->Command.AddPointer,
                   Tracker
                   );
        break;

      case QemuLoaderCmdAddChecksum:
        Status = ProcessCmdAddChecksum (
                   &LoaderEntry->Command.AddChecksum,
                   Tracker
                   );
        break;

      case QemuLoaderCmdWritePointer:
        Status = ProcessCmdWritePointer (
                   &LoaderEntry->Command.WritePointer,
                   Tracker,
                   S3Context
                   );
        if (!EFI_ERROR (Status)) {
          WritePointerSubsetEnd = LoaderEntry + 1;
        }

        break;

      default:
        DEBUG ((
          DEBUG_VERBOSE,
          "%a: unknown loader command: 0x%x\n",
          __func__,
          LoaderEntry->Type
          ));
        break;
    }

    if (EFI_ERROR (Status)) {
      goto RollbackWritePointersAndFreeTracker;
    }
  }

  InstalledKey = AllocatePool (INSTALLED_TABLES_MAX * sizeof *InstalledKey);
  if (InstalledKey == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto RollbackWritePointersAndFreeTracker;
  }

  SeenPointers = OrderedCollectionInit (PointerCompare, PointerCompare);
  if (SeenPointers == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeKeys;
  }

  //
  // second pass: identify and install ACPI tables
  //
  Installed = 0;
  for (LoaderEntry = LoaderStart; LoaderEntry < LoaderEnd; ++LoaderEntry) {
    if (LoaderEntry->Type == QemuLoaderCmdAddPointer) {
      Status = Process2ndPassCmdAddPointer (
                 &LoaderEntry->Command.AddPointer,
                 Tracker,
                 AcpiProtocol,
                 InstalledKey,
                 &Installed,
                 SeenPointers
                 );
      if (EFI_ERROR (Status)) {
        goto UninstallAcpiTables;
      }
    }
  }

  //
  // Install a protocol to notify that the ACPI table provided by Qemu is
  // ready.
  //
  QemuAcpiHandle = NULL;
  Status         = gBS->InstallProtocolInterface (
                          &QemuAcpiHandle,
                          &gQemuAcpiTableNotifyProtocolGuid,
                          EFI_NATIVE_INTERFACE,
                          NULL
                          );
  if (EFI_ERROR (Status)) {
    goto UninstallAcpiTables;
  }

   // modification: add additional SSDT
   UINT32 VromSize = 256*1024;
   UINT8* FwData = AllocateRuntimePool(VromSize); // 256*1024 = 256 KB = size of VROM image

   // copy VROM to FwData
   CopyMem (FwData, VROM_BIN, VROM_BIN_LEN);

   // header of SSDT table: DefinitionBlock ("Ssdt.aml", "SSDT", 1, "REDHAT", "OVMF    ", 1)
   unsigned char Ssdt_header[] = {
   0x53, 0x53, 0x44, 0x54, 0x24, 0x00, 0x00, 0x00, 0x01, 0x86, 0x52, 0x45,
   0x44, 0x48, 0x41, 0x54, 0x4f, 0x56, 0x4d, 0x46, 0x20, 0x20, 0x20, 0x20,
   0x01, 0x00, 0x00, 0x00, 0x49, 0x4e, 0x54, 0x4c, 0x31, 0x08, 0x16, 0x20
   };

   // byte 4-7: length header + table in little endian (so equal to SsdtSize)
   // byte 8: version complicance number: nothing needs to change
   // byte 9: set such that when all bytes are added modulo 256 the sum equals 0
   // calculate checksum: already implemented as CalculateCheckSum8(offset, length)

   unsigned int Ssdt_header_len = 36;

   UINTN                SsdtSize;
   UINT8                *Ssdt;

   SsdtSize = Ssdt_header_len + 17 + vrom_table_len;
   Ssdt = AllocatePool (SsdtSize);

   UINT8 *SsdtPtr = Ssdt;

   // copy header to Ssdt table
   CopyMem (SsdtPtr, Ssdt_header, Ssdt_header_len);
   SsdtPtr += Ssdt_header_len;

   // build "OperationRegion(FWDT, SystemMemory, 0x12345678, 0x87654321)"
   //
   *(SsdtPtr++) = 0x5B; // ExtOpPrefix
   *(SsdtPtr++) = 0x80; // OpRegionOp
   *(SsdtPtr++) = 'V';
   *(SsdtPtr++) = 'B';
   *(SsdtPtr++) = 'O';
   *(SsdtPtr++) = 'R';
   *(SsdtPtr++) = 0x00; // SystemMemory
   *(SsdtPtr++) = 0x0C; // DWordPrefix

   //
   // no virtual addressing yet, take the four least significant bytes
   //
   CopyMem(SsdtPtr, &FwData, 4);
   SsdtPtr += 4;

   *(SsdtPtr++) = 0x0C; // DWordPrefix

   *(UINT32*) SsdtPtr = VromSize;
   SsdtPtr += 4;

   CopyMem (SsdtPtr, vrom_table, vrom_table_len);

   // set the correct size in the header
   UINT32* size_ptr = (UINT32*) &Ssdt[4];
   *size_ptr = SsdtSize;

   // set byte 9 of header so the checksum equals 0
   Ssdt[9] = 0;
   UINT32 checksum = CalculateCheckSum8(Ssdt, SsdtSize);
   Ssdt[9] = (256 - checksum) % 256;

   Status = AcpiProtocol->InstallAcpiTable (AcpiProtocol,
                            (VOID *) Ssdt, SsdtSize,
                            &InstalledKey[Installed]);
   ++Installed;

  //
  // Translating the condensed QEMU_LOADER_WRITE_POINTER commands to ACPI S3
  // Boot Script opcodes has to be the last operation in this function, because
  // if it succeeds, it cannot be undone.
  //
  if (S3Context != NULL) {
    Status = TransferS3ContextToBootScript (S3Context);
    if (EFI_ERROR (Status)) {
      goto UninstallQemuAcpiTableNotifyProtocol;
    }

    //
    // Ownership of S3Context has been transferred.
    //
    S3Context = NULL;
  }

  DEBUG ((DEBUG_INFO, "%a: installed %d tables\n", __func__, Installed));

UninstallQemuAcpiTableNotifyProtocol:
  if (EFI_ERROR (Status)) {
    gBS->UninstallProtocolInterface (
           QemuAcpiHandle,
           &gQemuAcpiTableNotifyProtocolGuid,
           NULL
           );
  }

UninstallAcpiTables:
  if (EFI_ERROR (Status)) {
    //
    // roll back partial installation
    //
    while (Installed > 0) {
      --Installed;
      AcpiProtocol->UninstallAcpiTable (AcpiProtocol, InstalledKey[Installed]);
    }
  }

  for (SeenPointerEntry = OrderedCollectionMin (SeenPointers);
       SeenPointerEntry != NULL;
       SeenPointerEntry = SeenPointerEntry2)
  {
    SeenPointerEntry2 = OrderedCollectionNext (SeenPointerEntry);
    OrderedCollectionDelete (SeenPointers, SeenPointerEntry, NULL);
  }

  OrderedCollectionUninit (SeenPointers);

FreeKeys:
  FreePool (InstalledKey);

RollbackWritePointersAndFreeTracker:
  //
  // In case of failure, revoke any allocation addresses that were communicated
  // to QEMU previously, before we release all the blobs.
  //
  if (EFI_ERROR (Status)) {
    LoaderEntry = WritePointerSubsetEnd;
    while (LoaderEntry > LoaderStart) {
      --LoaderEntry;
      if (LoaderEntry->Type == QemuLoaderCmdWritePointer) {
        UndoCmdWritePointer (&LoaderEntry->Command.WritePointer);
      }
    }
  }

  //
  // Tear down the tracker infrastructure. Each fw_cfg blob will be left in
  // place only if we're exiting with success and the blob hosts data that is
  // not directly part of some ACPI table.
  //
  for (TrackerEntry = OrderedCollectionMin (Tracker); TrackerEntry != NULL;
       TrackerEntry = TrackerEntry2)
  {
    VOID  *UserStruct;
    BLOB  *Blob;

    TrackerEntry2 = OrderedCollectionNext (TrackerEntry);
    OrderedCollectionDelete (Tracker, TrackerEntry, &UserStruct);
    Blob = UserStruct;

    if (EFI_ERROR (Status) || Blob->HostsOnlyTableData) {
      DEBUG ((
        DEBUG_VERBOSE,
        "%a: freeing \"%a\"\n",
        __func__,
        Blob->File
        ));
      gBS->FreePages ((UINTN)Blob->Base, EFI_SIZE_TO_PAGES (Blob->Size));
    }

    FreePool (Blob);
  }

  OrderedCollectionUninit (Tracker);

FreeS3Context:
  if (S3Context != NULL) {
    ReleaseS3Context (S3Context);
  }

FreeAllocationsRestrictedTo32Bit:
  ReleaseAllocationsRestrictedTo32Bit (AllocationsRestrictedTo32Bit);

FreeLoader:
  FreePool (LoaderStart);

  return Status;
}
