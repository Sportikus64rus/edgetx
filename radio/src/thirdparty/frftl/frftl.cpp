/*
 * FrFTL - Flash Resident Flash Translation Layer
 * 
 * This is a FTL designed for NOR flash, logical to physical mapping uses 2 layers
 * of translation tables all resident in flash.  It comes with mechanisms to ensure
 * the integrity of the data in previous state when power out occurs in the middle
 * of flash programming.
 * 
 * It can be used to back the FatFS library by ChaN and included the support of
 * CTRL_SYNC and CTRL_TRIM functions for best performance.
 * 
 * Copyright (C) 2023 Dr. Richard Li <richard.li@ces.hk>
 *
 * License GPLv3: https://www.gnu.org/licenses/gpl-3.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frftl.h"
#include "crc.h"

#define SECTOR_SIZE                    512
#define PAGE_SIZE                      4096
#define SECTORS_PER_PAGE               (PAGE_SIZE / SECTOR_SIZE)
#define TT_PAGE_MAGIC                  0xEF87364A
#define TT_RECORDS_PER_PAGE            1024
#define BUFFER_SIZE_MULTIPLIER         4    // The multiplier for cache buffers, min recommendation is 2
#define RESERVED_PAGES_MULTIPLIER      16   // Reserve pages to minimize the erase cycles when the FS is full,
                                            // should be at least 2 times of BUFFER_SIZE_MULTIPLIER

typedef enum
{
  UNKNOWN,
  USED,
  ERASE_REQUIRED,
  ERASED
} PhysicalPageState;

typedef enum
{
  NONE,
  PROGRAM,
  ERASE_PROGRAM,
  RELOCATE_ERASE_PROGRAM
} ProgramMode;

typedef struct
{
  int16_t physicalPageNo;
  uint8_t sectStatus;
} __attribute__((__packed__)) PageInfo;

typedef struct
{
  uint32_t magicStart;
  uint32_t logicalPageNo;
  uint32_t serial;
  uint16_t padding;
  uint16_t crc16;
} __attribute__((__packed__)) TransTableHeader;

typedef struct
{
  TransTableHeader header;
  PageInfo pageInfos[TT_RECORDS_PER_PAGE];
} TransTable;

typedef union
{
  TransTable tt;
  uint8_t data[PAGE_SIZE];
} Page;

typedef struct
{
  int16_t logicalPageNo;  // required for first program or reprogram
  int16_t physicalPageNo;
  uint8_t lru;     // index for physicalPageNo, 0 is most used
  bool lock;       // page locked for delayed update
  ProgramMode pMode;
  Page page;
} __attribute__((__packed__)) PageBuffer;

static const uint8_t supportedFlashSizes[] = { 4, 8, 16, 32, 64, 128 };

PhysicalPageState getPhysicalPageState(FrFTL* ftl, int16_t physicalPageNo)
{
  uint32_t idx = physicalPageNo >> 4;
  uint32_t result = (ftl->physicalPageState[idx] >> ((physicalPageNo & 0xf) * 2));
  return (PhysicalPageState) (result & 0x3);
}

void setPhysicalPageState(FrFTL* ftl, int16_t physicalPageNo, PhysicalPageState state)
{
  uint32_t idx = physicalPageNo >> 4;
  uint32_t mask = 0x3 << ((physicalPageNo & 0xf) * 2);
  ftl->physicalPageState[idx] &= ~mask;
  ftl->physicalPageState[idx] |= ((state & 0x3) << ((physicalPageNo & 0xf) * 2));
}

static uint16_t calcCRC(TransTableHeader* header)
{
  header->padding = 0xffff;
  uint16_t crc = crc16(CRC_1021, (uint8_t*)header, sizeof(TransTableHeader) - 2, 0xffff);
  return crc;
}

static void resolveUnknownState(FrFTL* ftl, uint16_t count)
{
  if (ftl->physicalPageStateResolved)
  {
    return;
  }
  PhysicalPageState state;
  uint16_t idx = ftl->writeFrontier;
  bool earlyEnd = false;
  for (int i = 0; i < ftl->physicalPageCount; i++)
  {
    if (getPhysicalPageState(ftl, idx) == UNKNOWN)
    {
      PhysicalPageState state = ftl->isFlashErased(idx * PAGE_SIZE) ? ERASED : ERASE_REQUIRED;
      setPhysicalPageState(ftl, idx, state);
      count--;
      if (count == 0)
      {
        earlyEnd = true;
        break;
      }
    }
    idx++;
    if (idx >= ftl->physicalPageCount)
    {
      idx = 0;
    }
  }
  if (!earlyEnd)
  {
    ftl->physicalPageStateResolved = true;
  }
}

static PageBuffer* findPhysicalPageInBuffer(FrFTL* ftl, uint16_t physicalPageNo)
{
  PageBuffer* pageBuffer = ((PageBuffer*)(ftl->pageBuffer));
  for (uint8_t i = 0; i < ftl->pageBufferSize; i++)
  {
    PageBuffer* iBuffer = (pageBuffer + i);
    if (iBuffer->physicalPageNo == physicalPageNo)
    {
      // Found physical page in buffer
      // Update LRU
      for (uint8_t j = 0; j < ftl->pageBufferSize; j++)
      {
        if ((pageBuffer + j)->lru == i && j > 0)
        {
          uint8_t temp = i;
          while (j > 0)
          {
            (pageBuffer + j)->lru = (pageBuffer + j - 1)->lru;
            j--;
          }
          pageBuffer->lru = temp;
          break;
        }
      }

      return iBuffer;
    }
  }
  return NULL;
}

static PageBuffer* loadPhysicalPageInBuffer(FrFTL* ftl, uint16_t logicalPageNo, uint16_t physicalPageNo)
{
  // Find page in buffer
  PageBuffer* pageBuffer = ((PageBuffer*)(ftl->pageBuffer));
  PageBuffer* currentBuffer = findPhysicalPageInBuffer(ftl, physicalPageNo);
  if (currentBuffer == NULL)
  {
    // Page not in buffer, load page in buffer
    uint8_t bufferIdx = 0;
    for (int i = ftl->pageBufferSize - 1; i >= 0; i--)
    {
      bufferIdx = (pageBuffer + i)->lru;
      currentBuffer = (pageBuffer + bufferIdx);
      if (!currentBuffer->lock)
      {
        break;
      }
    }
    if (currentBuffer->lock)
    {
      return NULL;
    }
    currentBuffer->physicalPageNo = -1;
    if (!ftl->flashRead(physicalPageNo * PAGE_SIZE, currentBuffer->page.data, PAGE_SIZE))
    {
      return NULL;
    }
    currentBuffer->logicalPageNo = logicalPageNo;
    currentBuffer->physicalPageNo = physicalPageNo;
    currentBuffer->lock = false;
    currentBuffer->pMode = NONE;

    // Update LRU
    for (uint8_t j = 0; j < ftl->pageBufferSize; j++)
    {
      if ((pageBuffer + j)->lru == bufferIdx && j > 0)
      {
        uint8_t temp = bufferIdx;
        while (j > 0)
        {
          (pageBuffer + j)->lru = (pageBuffer + j - 1)->lru;
          j--;
        }
        pageBuffer->lru = temp;
        break;
      }
    }
  }

  return currentBuffer;
}

static PageBuffer* initPhysicalPageInBuffer(FrFTL* ftl, uint16_t logicalPageNo, uint16_t physicalPageNo)
{
  // Find page in buffer
  PageBuffer* currentBuffer = findPhysicalPageInBuffer(ftl, physicalPageNo);
  if (currentBuffer == NULL)
  {
    // Page not in buffer, load page in buffer
    PageBuffer* pageBuffer = ((PageBuffer*)(ftl->pageBuffer));
    uint8_t bufferIdx;
    for (int i = ftl->pageBufferSize - 1; i >= 0; i--)
    {
      bufferIdx = (pageBuffer + i)->lru;
      currentBuffer = (pageBuffer + bufferIdx);
      if (!currentBuffer->lock)
      {
        break;
      }
    }
    if (currentBuffer->lock)
    {
      return NULL;
    }
    currentBuffer->logicalPageNo = logicalPageNo;
    currentBuffer->physicalPageNo = physicalPageNo;
    currentBuffer->lock = true;
    currentBuffer->pMode = ERASE_PROGRAM;

    memset(currentBuffer->page.data, 0xff, PAGE_SIZE);
  }

  return currentBuffer;
}

static bool hasFreeBuffers(FrFTL* ftl, uint16_t bufferCount)
{
  uint16_t freeBufferFound = 0;
  PageBuffer* pageBuffer = ((PageBuffer*)(ftl->pageBuffer));
  for (uint8_t i = 0; i < ftl->pageBufferSize; i++)
  {
    if (!(pageBuffer + i)->lock)
    {
      freeBufferFound++;
      if (freeBufferFound >= bufferCount)
      {
        return true;
      }
    }
  }
  return false;
}

static bool readPhysicalSector(FrFTL* ftl, uint8_t* buffer, uint16_t logicalPageNo, uint16_t physicalPageNo, uint8_t pageSectorNo)
{
  PageBuffer* pageBuffer = loadPhysicalPageInBuffer(ftl, logicalPageNo, physicalPageNo);
  if (pageBuffer != NULL)
  {
    memcpy(buffer, pageBuffer->page.data + pageSectorNo * SECTOR_SIZE, SECTOR_SIZE);
    return true;
  }
  return false;
}

static bool readPhysicalPageInfo(FrFTL* ftl, PageInfo* pageInfo, uint16_t logicalPageNo, uint16_t physicalPageNo, uint16_t recordNo)
{
//  printf("readPhysicalPageInfo: %d, %d", physicalPageNo, recordNo);
  PageBuffer* pageBuffer = loadPhysicalPageInBuffer(ftl, logicalPageNo, physicalPageNo);
  if (pageBuffer != NULL)
  {
    // Page found in buffer
    memcpy(pageInfo, &pageBuffer->page.tt.pageInfos[recordNo], sizeof(PageInfo));
    return true;
  }
  return false;
}

static bool readPageInfo(FrFTL* ftl, PageInfo* pageInfo, uint16_t logicalPageNo)
{
  if (logicalPageNo < TT_RECORDS_PER_PAGE)
  {
    // Read from master TT
    return readPhysicalPageInfo(ftl, pageInfo, 0, ftl->mttPhysicalPageNo, logicalPageNo);
  }
  else
  {
    // Lookup from secondary TT from master TT
    PageInfo secondaryTTInfo;
    uint16_t sttLogicalPageNo = logicalPageNo / TT_RECORDS_PER_PAGE;
    if (!readPhysicalPageInfo(ftl, &secondaryTTInfo, 0, ftl->mttPhysicalPageNo, sttLogicalPageNo))
    {
      return false;
    }

    // Read from secondary TT
    return readPhysicalPageInfo(ftl, pageInfo, sttLogicalPageNo, secondaryTTInfo.physicalPageNo, logicalPageNo % TT_RECORDS_PER_PAGE);
  }
}

static bool updatePhysicalPageInfo(FrFTL* ftl, PageInfo* pageInfo, uint16_t logicalPageNo, uint16_t physicalPageNo, uint16_t recordNo)
{
  PageBuffer* pageBuffer = loadPhysicalPageInBuffer(ftl, logicalPageNo, physicalPageNo);
  if (!pageBuffer)
  {
    return false;
  }

  // Update info, need to lock and ensure update
  pageBuffer->lock = true;
  if (pageBuffer->pMode == NONE)
  {
    pageBuffer->pMode = PROGRAM;
  }
  memcpy(&pageBuffer->page.tt.pageInfos[recordNo], pageInfo, sizeof(PageInfo));

  return true;
}

static bool updatePageInfo(FrFTL* ftl, PageInfo* pageInfo, uint16_t logicalPageNo)
{
  if (logicalPageNo < TT_RECORDS_PER_PAGE)
  {
    // Update to master TT
    return updatePhysicalPageInfo(ftl, pageInfo, 0, ftl->mttPhysicalPageNo, logicalPageNo);
  }
  else
  {
    // Lookup from secondary TT from master TT
    PageInfo secondaryTTInfo;
    uint16_t sttLogicalPageNo = logicalPageNo / TT_RECORDS_PER_PAGE;
    if (!readPhysicalPageInfo(ftl, &secondaryTTInfo, 0, ftl->mttPhysicalPageNo, sttLogicalPageNo))
    {
      return false;
    }

    // Program to secondary TT
    return updatePhysicalPageInfo(ftl, pageInfo, sttLogicalPageNo, secondaryTTInfo.physicalPageNo, logicalPageNo % TT_RECORDS_PER_PAGE);
  }
}

static int16_t allocatePhysicalPage(FrFTL* ftl)
{
  uint16_t lookupCount = 0;
  while (getPhysicalPageState(ftl, ftl->writeFrontier) == USED)
  {
    ftl->writeFrontier++;
    if (ftl->writeFrontier >= ftl->physicalPageCount)
    {
      ftl->writeFrontier = 0;
    }
    lookupCount++;
    if (lookupCount > ftl->physicalPageCount)
    {
      printf("BUG: writeFrontier = %d\n", ftl->writeFrontier);
      return -1;  // BUG
    }
  }

  uint16_t physicalPageNo = ftl->writeFrontier++;
  if (ftl->writeFrontier >= ftl->physicalPageCount)
  {
    ftl->writeFrontier = 0;
  }

  return physicalPageNo;
}

static bool programPageInBuffer(FrFTL* ftl, PageBuffer* buffer)
{
  uint32_t addr;
  int16_t newPhysicalPageNo;
  switch (buffer->pMode)
  {
    case PROGRAM:
      // Program only
      if (!ftl->flashProgram(buffer->physicalPageNo * PAGE_SIZE, buffer->page.data, PAGE_SIZE))
      {
        return false;
      }
      setPhysicalPageState(ftl, buffer->physicalPageNo, USED);
      break;
    case ERASE_PROGRAM:
      addr = buffer->physicalPageNo * PAGE_SIZE;
      if (getPhysicalPageState(ftl, buffer->physicalPageNo) != ERASED)
      {
        // Do erase on the fly
        if (!ftl->flashErase(addr))
        {
          return false;
        }
      }
      if (!ftl->flashProgram(addr, buffer->page.data, PAGE_SIZE))
      {
        return false;
      }
      setPhysicalPageState(ftl, buffer->physicalPageNo, USED);
      break;
    case RELOCATE_ERASE_PROGRAM:
      // Reprogram
      newPhysicalPageNo = allocatePhysicalPage(ftl);
      if (newPhysicalPageNo < 0)
      {
        return false;
      }

      if (buffer->logicalPageNo < ftl->ttPageCount)
      {
        if (buffer->logicalPageNo == 0)
        {
          // MTT need update physicalPageNo
          buffer->page.tt.pageInfos[0].physicalPageNo = newPhysicalPageNo;
        }

        // TT page, need update serial and CRC
        buffer->page.tt.header.serial++;
        buffer->page.tt.header.crc16 = calcCRC(&buffer->page.tt.header);
      }

      addr = newPhysicalPageNo * PAGE_SIZE;
      if (getPhysicalPageState(ftl, buffer->physicalPageNo) != ERASED)
      {
        // Do erase on the fly
        if (!ftl->flashErase(addr))
        {
          return false;
        }
      }
      if (!ftl->flashProgram(addr, buffer->page.data, PAGE_SIZE))
      {
        return false;
      }
      setPhysicalPageState(ftl, buffer->physicalPageNo, ERASE_REQUIRED);
      buffer->physicalPageNo = newPhysicalPageNo;
      setPhysicalPageState(ftl, buffer->physicalPageNo, USED);
      if (buffer->logicalPageNo == 0)
      {
        // MTT page, need update mttPhysicalPageNo
        ftl->mttPhysicalPageNo = buffer->physicalPageNo;
      }
      break;
  }
  return true;
}

bool ftlSync(FrFTL* ftl)
{
  PageBuffer* pageBuffer = ((PageBuffer*)(ftl->pageBuffer));

  // First program data pages
  for (int i = 0; i < ftl->pageBufferSize; i++)
  {
    PageBuffer* currentBuffer = pageBuffer + i;
    if (currentBuffer->lock)
    {
      if (currentBuffer->logicalPageNo >= ftl->ttPageCount)
      {
        if (!programPageInBuffer(ftl, currentBuffer))
        {
          return false;
        }

        // Update PageInfo in TT pages
        PageInfo pageInfo;
        if (!readPageInfo(ftl, &pageInfo, currentBuffer->logicalPageNo))
        {
          return false;
        }
        pageInfo.physicalPageNo = currentBuffer->physicalPageNo;
        if (!updatePageInfo(ftl, &pageInfo, currentBuffer->logicalPageNo))
        {
          return false;
        }

        // Unlock buffer
        currentBuffer->lock = false;
        currentBuffer->pMode = NONE;
      }
    }
  }

  // Second program STT pages
  PageBuffer* mttBuffer = loadPhysicalPageInBuffer(ftl, 0, ftl->mttPhysicalPageNo);
  if (!mttBuffer)
  {
    return false;
  }
  for (int i = 0; i < ftl->pageBufferSize; i++)
  {
    PageBuffer* currentBuffer = pageBuffer + i;
    if (currentBuffer->lock)
    {
      if (currentBuffer->logicalPageNo > 0 && currentBuffer->logicalPageNo < ftl->ttPageCount)
      {
        if (!programPageInBuffer(ftl, currentBuffer))
        {
          return false;
        }

        // Update PageInfo in MTT page
        mttBuffer->page.tt.pageInfos[currentBuffer->logicalPageNo].physicalPageNo = currentBuffer->physicalPageNo;

        // Unlock buffer
        currentBuffer->lock = false;
        currentBuffer->pMode = NONE;
      }
    }
  }

  // Finally program MTT page
  if (mttBuffer->lock)
  {
    if (!programPageInBuffer(ftl, mttBuffer))
    {
      return false;
    }

    // Unlock buffer
    mttBuffer->lock = false;
    mttBuffer->pMode = NONE;
  }

  return true;
}

bool ftlWriteSector(FrFTL* ftl, uint32_t startSectorNo, uint32_t noOfSectors, const uint8_t* buf)
{
  resolveUnknownState(ftl, ftl->ttPageCount);
  if (startSectorNo + noOfSectors > ftl->usableSectorCount)
  {
    return false;
  }

  uint32_t sectorNo = startSectorNo;
  while (noOfSectors > 0)
  {
    if (!hasFreeBuffers(ftl, 3))  // Max no. of sectors need to be rewritten is 3, need to ensure has enough free buffers
    {
      // Flush the buffers first if free space is not found
      if (!ftlSync(ftl))
      {
        return false;
      }
    }

    uint16_t logicalPageNo = sectorNo / SECTORS_PER_PAGE + ftl->ttPageCount;
    uint8_t pageSectorNo = sectorNo % SECTORS_PER_PAGE;

    // Read page info
    PageInfo pageInfo;
    readPageInfo(ftl, &pageInfo, logicalPageNo);
    PageBuffer* dataBuffer;

    // Allocate new physical page for uninitialized logical page
    if (pageInfo.physicalPageNo < 0)
    {
      // Need allocate physical page
      if ((pageInfo.physicalPageNo = allocatePhysicalPage(ftl)) < 0)
      {
        return false;
      }

      // Init page in in buffer, locked for delayed program
      dataBuffer = initPhysicalPageInBuffer(ftl, logicalPageNo, pageInfo.physicalPageNo);
      if (!dataBuffer)
      {
        return false;
      }

      pageInfo.sectStatus = 0xff;

      if (!updatePageInfo(ftl, &pageInfo, logicalPageNo))
      {
        return false;
      }
    }
    else
    {
      dataBuffer = loadPhysicalPageInBuffer(ftl, logicalPageNo, pageInfo.physicalPageNo);
      if (!dataBuffer)
      {
        return false;
      }
    }

    uint8_t sectMask = 1 << pageSectorNo;
    if ((pageInfo.sectStatus & sectMask) != 0)
    {
      // Sector never write, append information
      pageInfo.sectStatus &= ~sectMask;
      if (!updatePageInfo(ftl, &pageInfo, logicalPageNo))
      {
        return false;
      }

      // Update sector, locked for delayed program
      dataBuffer->lock = true;
      if (dataBuffer->pMode == NONE)
      {
        dataBuffer->pMode = PROGRAM;
      }
      memcpy(dataBuffer->page.data + pageSectorNo * SECTOR_SIZE, buf, SECTOR_SIZE);
    }
    else
    {
      // Sector already written, use replace write
      // Lock page for delayed update with reprogram
      dataBuffer->lock = true;
      dataBuffer->pMode = RELOCATE_ERASE_PROGRAM;
      memcpy(dataBuffer->page.data + pageSectorNo * SECTOR_SIZE, buf, SECTOR_SIZE);

      // Read TT pages and lock it for later update
      PageBuffer* ttBuffer;
      PageInfo ttPageInfo;
      uint16_t ttPageNo = logicalPageNo / TT_RECORDS_PER_PAGE;
      if (!readPageInfo(ftl, &ttPageInfo, ttPageNo))
      {
        return false;
      }
      ttBuffer = loadPhysicalPageInBuffer(ftl, ttPageNo, ttPageInfo.physicalPageNo);
      ttBuffer->lock = true;
      ttBuffer->pMode = RELOCATE_ERASE_PROGRAM;
      if (ttPageNo > 0)
      {
        ttBuffer = loadPhysicalPageInBuffer(ftl, 0, ftl->mttPhysicalPageNo);
        ttBuffer->lock = true;
        ttBuffer->pMode = RELOCATE_ERASE_PROGRAM;
      }
    }

    noOfSectors--;
    sectorNo++;
    buf += SECTOR_SIZE;
  }

  return true;
}

bool ftlReadSector(FrFTL* ftl, uint32_t sectorNo, uint8_t* buffer)
{
//  doGC(ftl, ftl->ttPageCount, 1);
  if (sectorNo >= ftl->usableSectorCount)
  {
    return false;
  }

  uint16_t logicalPageNo = sectorNo / SECTORS_PER_PAGE + ftl->ttPageCount;
  uint8_t pageSectorNo = sectorNo % SECTORS_PER_PAGE;

  // Read page info
  PageInfo pageInfo;
  readPageInfo(ftl, &pageInfo, logicalPageNo);

  // Check if sector written before
  uint8_t sectMask = 1 << pageSectorNo;
  if ((pageInfo.sectStatus & sectMask) != 0)
  {
    // Sector never write, return init content
    memset(buffer, 0xff, SECTOR_SIZE);
    return true;
  }
  else
  {
    return readPhysicalSector(ftl, buffer, logicalPageNo, pageInfo.physicalPageNo, pageSectorNo);
  }
}

static void initPageBuffer(FrFTL* ftl)
{
  size_t bufferSize = sizeof(PageBuffer) * ftl->pageBufferSize;  
  ftl->memoryUsed += bufferSize;
  ftl->pageBuffer = (PageBuffer*)malloc(bufferSize);
  for (int8_t i = 0; i < ftl->pageBufferSize; i++)
  {
    PageBuffer* currentBuffer = ((PageBuffer *) ftl->pageBuffer) + i;
    currentBuffer->logicalPageNo = -1;
    currentBuffer->physicalPageNo = -1;
    currentBuffer->lru = i;
    currentBuffer->lock = false;
    currentBuffer->pMode = NONE;
  }
}

static void initTransTablePage(FrFTL* ftl, Page* page, uint32_t logicalPageNo)
{
  memset(page->data, 0xff, PAGE_SIZE);
  page->tt.header.magicStart = TT_PAGE_MAGIC;
  page->tt.header.logicalPageNo = logicalPageNo;
  page->tt.header.serial = 1;
  page->tt.header.crc16 = calcCRC(&page->tt.header);
}

void createFTL(FrFTL* ftl)
{
  // Resolve the first few blocks for proper startup
  ftl->writeFrontier = 0;
  resolveUnknownState(ftl, ftl->pageBufferSize);

  Page mtt;
  initTransTablePage(ftl, &mtt, 0);
  mtt.tt.pageInfos[0].physicalPageNo = 0;
  mtt.tt.pageInfos[0].sectStatus = 0;

  Page stt;
  for (int i = 1; i < ftl->ttPageCount; i++)
  {
    initTransTablePage(ftl, &stt, i);
    uint32_t addr = i * PAGE_SIZE;
    if (getPhysicalPageState(ftl, i) != ERASED)
    {
      ftl->flashErase(addr);
    }
    ftl->flashProgram(addr, stt.data, PAGE_SIZE);
    setPhysicalPageState(ftl, i, USED);
    mtt.tt.pageInfos[i].physicalPageNo = i;
    mtt.tt.pageInfos[i].sectStatus = 0;
  }

  if (getPhysicalPageState(ftl, 0) != ERASED)
  {
    ftl->flashErase(0);
  }
  ftl->flashProgram(0, mtt.data, PAGE_SIZE);
  setPhysicalPageState(ftl, 0, USED);

  ftl->writeFrontier = ftl->ttPageCount;
}

static bool loadFTL(FrFTL* ftl)
{
  // Scan for MTT
  uint32_t currentSerial = 0;
  int16_t currentPhysicalMTTPageNo = -1;
  for (int16_t i = 0; i < ftl->physicalPageCount; i++)
  {
    TransTableHeader header;
    ftl->flashRead(i * PAGE_SIZE, (uint8_t*)&header, sizeof(header));
    if (header.magicStart == TT_PAGE_MAGIC && header.logicalPageNo == 0 && header.crc16 == calcCRC(&header))
    {
      // MTT detected
      if (header.serial > currentSerial)
      {
        // Newer MTT found
        currentSerial = header.serial;
        currentPhysicalMTTPageNo = i;
      }
    }
  }

  if (currentPhysicalMTTPageNo >= 0)
  {
    // MTT found, load data
    ftl->mttPhysicalPageNo = currentPhysicalMTTPageNo;
    setPhysicalPageState(ftl, currentPhysicalMTTPageNo, USED);
    ftl->writeFrontier = currentPhysicalMTTPageNo + 1;
    if (ftl->writeFrontier >= ftl->physicalPageCount)
    {
      ftl->writeFrontier = 0;
    }

    PageBuffer* mtt = loadPhysicalPageInBuffer(ftl, 0, currentPhysicalMTTPageNo);
    for (int i = 1; i < TT_RECORDS_PER_PAGE; i++)
    {
      int16_t currentPhysicalPageNo = mtt->page.tt.pageInfos[i].physicalPageNo;
      if (currentPhysicalPageNo >= 0)
      {
        // Used page
        setPhysicalPageState(ftl, currentPhysicalPageNo, USED);
      }
      if (i < ftl->ttPageCount)
      {
        // TT pages
        PageBuffer* stt = loadPhysicalPageInBuffer(ftl, i, currentPhysicalPageNo);
        for (int j = 0; j < TT_RECORDS_PER_PAGE; j++)
        {
          currentPhysicalPageNo = stt->page.tt.pageInfos[j].physicalPageNo;
          if (currentPhysicalPageNo >= 0)
          {
            // Used page
            setPhysicalPageState(ftl, currentPhysicalPageNo, USED);
          }
        }
      }
    }

    // Walk forward to ensure some pages are resolved
    resolveUnknownState(ftl, ftl->pageBufferSize);
    return true;
  }

  return false;
}

FrFTL* ftlInit(FlashReadCB rf, FlashProgramCB pf, FlashEraseCB ef, IsFlashErasedCB ief, uint8_t flashSizeInMB)
{
  // Check flash size
  bool found = false;
  for (uint8_t i = 0; i < sizeof(supportedFlashSizes); i++)
  {
    if (flashSizeInMB == supportedFlashSizes[i])
    {
      found = true;
      break;
    }
  }
  if (!found)
  {
    return NULL;
  }

  FrFTL* ftl = (FrFTL*)calloc(sizeof(FrFTL), 1);
  ftl->memoryUsed = sizeof(FrFTL);
  ftl->flashRead = rf;
  ftl->flashProgram = pf;
  ftl->flashErase = ef;
  ftl->isFlashErased = ief;
  ftl->mttPhysicalPageNo = 0;
  ftl->physicalPageCount = flashSizeInMB * 1024 * 1024 / PAGE_SIZE;
  ftl->ttPageCount = ftl->physicalPageCount / TT_RECORDS_PER_PAGE;
  ftl->usableSectorCount = (ftl->physicalPageCount - ftl->ttPageCount * RESERVED_PAGES_MULTIPLIER) * SECTORS_PER_PAGE;
  uint32_t stateSize = ftl->physicalPageCount / 16 + (ftl->physicalPageCount % 16 > 0 ? 1 : 0);
  ftl->physicalPageState = (uint32_t*)calloc(stateSize, sizeof(uint32_t));
  ftl->physicalPageStateResolved = false;
  ftl->memoryUsed += stateSize * sizeof(uint32_t);
  ftl->pageBufferSize = ftl->ttPageCount * BUFFER_SIZE_MULTIPLIER;
  initPageBuffer(ftl);

  if (!loadFTL(ftl))
  {
    createFTL(ftl);
  }
  return ftl;
}

void ftlDeInit(FrFTL* ftl)
{
  free(ftl->pageBuffer);
  free(ftl->physicalPageState);
  free(ftl);
}

