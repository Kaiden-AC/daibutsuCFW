#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/types.h>
#include "common.h"
#include <hfs/hfsplus.h>
#include <hfs/hfscompress.h>
#include "abstractfile.h"
#include <sys/stat.h>
#include <inttypes.h>

#define BUFSIZE 1024*1024

static int silence = 0;

void hfs_setsilence(int s) {
	silence = s;
}

void writeToFile(HFSPlusCatalogFile* file, AbstractFile* output, Volume* volume) {
	unsigned char* buffer;
	io_func* io;
	off_t curPosition;
	size_t bytesLeft;
	
	buffer = (unsigned char*) malloc(BUFSIZE);

	if(file->permissions.ownerFlags & UF_COMPRESSED) {
		io = openHFSPlusCompressed(volume, file);
		if(io == NULL) {
			hfs_panic("error opening file");
			free(buffer);
			return;
		}

		curPosition = 0;
		bytesLeft = ((HFSPlusCompressed*) io->data)->decmpfs->size;
	} else {
		io = openRawFile(file->fileID, &file->dataFork, (HFSPlusCatalogRecord*)file, volume);
		if(io == NULL) {
			hfs_panic("error opening file");
			free(buffer);
			return;
		}

		curPosition = 0;
		bytesLeft = file->dataFork.logicalSize;
	}	
	while(bytesLeft > 0) {
		if(bytesLeft > BUFSIZE) {
			if(!READ(io, curPosition, BUFSIZE, buffer)) {
				hfs_panic("error reading");
			}
			if(output->write(output, buffer, BUFSIZE) != BUFSIZE) {
				hfs_panic("error writing");
			}
			curPosition += BUFSIZE;
			bytesLeft -= BUFSIZE;
		} else {
			if(!READ(io, curPosition, bytesLeft, buffer)) {
				hfs_panic("error reading");
			}
			if(output->write(output, buffer, bytesLeft) != bytesLeft) {
				hfs_panic("error writing");
			}
			curPosition += bytesLeft;
			bytesLeft -= bytesLeft;
		}
	}
	CLOSE(io);

	free(buffer);
}

void writeToHFSFile(HFSPlusCatalogFile* file, AbstractFile* input, Volume* volume) {
	unsigned char *buffer;
	io_func* io;
	off_t curPosition;
	off_t bytesLeft;

	buffer = (unsigned char*) malloc(BUFSIZE);
	
	bytesLeft = input->getLength(input);

	if(file->permissions.ownerFlags & UF_COMPRESSED) {
		io = openHFSPlusCompressed(volume, file);
		if(io == NULL) {
			hfs_panic("error opening file");
			free(buffer);
			return;
		}
		if (((HFSPlusCompressed *)io->data)->decmpfs->size > bytesLeft) {
			((HFSPlusCompressed *)io->data)->decmpfs->size = bytesLeft; /* XXX Probably a bad idea: truncate */
		}
	} else {
		io = openRawFile(file->fileID, &file->dataFork, (HFSPlusCatalogRecord*)file, volume);
		if(io == NULL) {
			hfs_panic("error opening file");
			free(buffer);
			return;
		}
		allocate((RawFile*)io->data, bytesLeft);
	}
	
	curPosition = 0;	
	
	while(bytesLeft > 0) {
		if(bytesLeft > BUFSIZE) {
			if(input->read(input, buffer, BUFSIZE) != BUFSIZE) {
				hfs_panic("error reading");
			}
			if(!WRITE(io, curPosition, BUFSIZE, buffer)) {
				hfs_panic("error writing");
			}
			curPosition += BUFSIZE;
			bytesLeft -= BUFSIZE;
		} else {
			if(input->read(input, buffer, (size_t)bytesLeft) != (size_t)bytesLeft) {
				hfs_panic("error reading");
			}
			if(!WRITE(io, curPosition, (size_t)bytesLeft, buffer)) {
				hfs_panic("error reading");
			}
			curPosition += bytesLeft;
			bytesLeft -= bytesLeft;
		}
	}

	CLOSE(io);

	free(buffer);
}

char* getLinkName(Volume* volume, HFSPlusCatalogFile* file) {
	if ((file->permissions.fileMode & 0170000) == 0120000) {
		size_t bufferSize = 32;
		void *buffer = malloc(bufferSize);
		AbstractFile* outFile = createAbstractFileFromMemory((void**)&buffer, bufferSize);
		if (outFile) {
			char zero = 0;
			writeToFile(file, outFile, volume);
			outFile->write(outFile, &zero, sizeof(zero));
			outFile->close(outFile);
			return buffer;
		}
		free(buffer);
	}
	return NULL;
}

void get_hfs(Volume* volume, const char* inFileName, AbstractFile* output) {
	HFSPlusCatalogRecord* record;
	
	record = getRecordFromPath(inFileName, volume, NULL, NULL);
	
	if(record != NULL) {
		if(record->recordType == kHFSPlusFileRecord)
			writeToFile((HFSPlusCatalogFile*)record,  output, volume);
		else {
			printf("Not a file\n");
			exit(0);
		}
	} else {
		printf("No such file or directory\n");
		exit(0);
	}
	
	free(record);
}

int add_hfs(Volume* volume, AbstractFile* inFile, const char* outFileName) {
	HFSPlusCatalogRecord* record;
	int ret;
	
	record = getRecordFromPath(outFileName, volume, NULL, NULL);
	
	if(record != NULL) {
		if(record->recordType == kHFSPlusFileRecord) {
			writeToHFSFile((HFSPlusCatalogFile*)record, inFile, volume);
			ret = TRUE;
		} else {
			printf("Not a file\n");
			exit(0);
		}
	} else {
		if(newFile(outFileName, volume)) {
			record = getRecordFromPath(outFileName, volume, NULL, NULL);
			writeToHFSFile((HFSPlusCatalogFile*)record, inFile, volume);
			ret = TRUE;
		} else {
			ret = FALSE;
		}
	}
	
	inFile->close(inFile);
	if(record != NULL) {
		free(record);
	}
	
	return ret;
}

void grow_hfs(Volume* volume, uint64_t newSize) {
	uint32_t newBlocks;
	uint32_t blocksToGrow;
	uint64_t newMapSize;
	uint64_t i;
	unsigned char zero;
	
	zero = 0;	
	
	newBlocks = newSize / volume->volumeHeader->blockSize;
	
	if(newBlocks <= volume->volumeHeader->totalBlocks) {
		printf("Cannot shrink volume\n");
		return;
	}

	blocksToGrow = newBlocks - volume->volumeHeader->totalBlocks;
	newMapSize = newBlocks / 8;
	
	if(volume->volumeHeader->allocationFile.logicalSize < newMapSize) {
		if(volume->volumeHeader->freeBlocks
		   < ((newMapSize - volume->volumeHeader->allocationFile.logicalSize) / volume->volumeHeader->blockSize)) {
			printf("Not enough room to allocate new allocation map blocks\n");
			exit(0);
		}
		
		allocate((RawFile*) (volume->allocationFile->data), newMapSize);
	}
	
	/* unreserve last block */	
	setBlockUsed(volume, volume->volumeHeader->totalBlocks - 1, 0);
	/* don't need to increment freeBlocks because we will allocate another alternate volume header later on */
	
	/* "unallocate" the new blocks */
	for(i = ((volume->volumeHeader->totalBlocks / 8) + 1); i < newMapSize; i++) {
		ASSERT(WRITE(volume->allocationFile, i, 1, &zero), "WRITE");
	}
	
	/* grow backing store size */
	ASSERT(WRITE(volume->image, newSize - 1, 1, &zero), "WRITE");
	
	/* write new volume information */
	volume->volumeHeader->totalBlocks = newBlocks;
	volume->volumeHeader->freeBlocks += blocksToGrow;
	
	/* reserve last block */	
	setBlockUsed(volume, volume->volumeHeader->totalBlocks - 1, 1);
	
	updateVolume(volume);
}

/* reads the whole allocation bitmap into memory; caller frees */
static unsigned char* readAllocationBitmap(Volume* volume, uint64_t* outSize) {
	uint64_t logicalSize = volume->volumeHeader->allocationFile.logicalSize;
	unsigned char* map;
	uint64_t offset;

	if(logicalSize == 0) {
		return NULL;
	}

	map = (unsigned char*) malloc(logicalSize);
	if(map == NULL) {
		return NULL;
	}

	offset = 0;
	while(offset < logicalSize) {
		size_t chunk = (logicalSize - offset) > (1024 * 1024) ? (1024 * 1024) : (size_t)(logicalSize - offset);
		if(!READ(volume->allocationFile, offset, chunk, map + offset)) {
			free(map);
			return NULL;
		}
		offset += chunk;
	}

	*outSize = logicalSize;
	return map;
}

/* returns the first used block in [startBlock, endBlock), or -1 if there is none */
static int64_t bitmapFindUsedInRange(const unsigned char* map, uint64_t mapSize, uint32_t startBlock, uint32_t endBlock) {
	uint32_t byteStart = startBlock / 8;
	uint32_t byteEnd = (endBlock + 7) / 8;
	uint32_t i;

	if((uint64_t)byteEnd > mapSize) {
		byteEnd = (uint32_t) mapSize;
	}
	if((uint64_t)byteStart >= mapSize) {
		return -1;
	}

	for(i = byteStart; i < byteEnd; i++) {
		unsigned char byte = map[i];
		uint32_t validBits = 8;

		if(i == (endBlock - 1) / 8 && (endBlock % 8) != 0) {
			validBits = endBlock % 8;
		}
		if(i == byteStart && (startBlock % 8) != 0) {
			byte &= (unsigned char)(0xFF >> (startBlock % 8));
		}
		if(validBits < 8) {
			byte &= (unsigned char)(0xFF << (8 - validBits));
		}

		if(byte != 0) {
			int b;
			for(b = 7; b >= 0; b--) {
				if(byte & (1 << b)) {
					uint32_t block = (i * 8) + (uint32_t)(7 - b);
					if(block >= startBlock && block < endBlock) {
						return block;
					}
				}
			}
		}
	}

	return -1;
}

/* returns the highest used block below limitBlock, or -1 if there is none */
static int64_t bitmapHighestUsed(const unsigned char* map, uint64_t mapSize, uint32_t limitBlock) {
	uint32_t byteIdx;
	uint32_t validBits;

	if(limitBlock == 0 || mapSize == 0) {
		return -1;
	}

	if((uint64_t) limitBlock > mapSize * 8) {
		limitBlock = (uint32_t) (mapSize * 8);
	}

	byteIdx = (limitBlock - 1) / 8;
	validBits = limitBlock - (byteIdx * 8);

	for(;;) {
		unsigned char byte = map[byteIdx];

		if(validBits < 8) {
			byte &= (unsigned char)(0xFF << (8 - validBits));
		}

		if(byte != 0) {
			int b;
			for(b = 0; b <= 7; b++) {
				/* block = byteIdx * 8 + (7 - b); b counts up from 0 so the first
				   match is the highest block in the byte */
				if(byte & (1 << b)) {
					uint32_t block = (byteIdx * 8) + (uint32_t)(7 - b);
					if(block < limitBlock) {
						return block;
					}
				}
			}
		}

		if(byteIdx == 0) {
			break;
		}
		byteIdx--;
		validBits = 8;
	}

	return -1;
}

/* The actual work for shrinking a volume. newBlocks must be >= 2 and < the current totalBlocks.
   Callers are expected to have sanity-checked newBlocks, but the bitmap is re-validated here so
   that no live data is ever clobbered. The backing store is truncated last, after all metadata
   is consistent. */
static int shrinkVolume(Volume* volume, uint32_t newBlocks) {
	HFSPlusVolumeHeader* volumeHeader = volume->volumeHeader;
	uint32_t blockSize = volumeHeader->blockSize;
	uint32_t oldTotalBlocks = volumeHeader->totalBlocks;
	uint64_t newSize = (uint64_t) newBlocks * (uint64_t) blockSize;
	uint64_t newMapSize = ((uint64_t) newBlocks + 7) / 8;
	unsigned char* map;
	uint64_t mapSize;
	int64_t used;

	if(newBlocks >= oldTotalBlocks) {
		printf("Cannot shrink volume: new size is not smaller than the current size\n");
		return FALSE;
	}

	/* pre-flight: make sure the backing store can be truncated (a negative size is a no-op probe) */
	if(!TRUNCATE(volume->image, -1)) {
		printf("Cannot shrink volume: image is read-only (compressed or partition-mapped DMGs cannot be written)\n");
		return FALSE;
	}

	map = readAllocationBitmap(volume, &mapSize);
	if(map == NULL) {
		printf("Cannot shrink volume: cannot read the allocation file\n");
		return FALSE;
	}

	if(mapSize < ((uint64_t) oldTotalBlocks + 7) / 8) {
		free(map);
		printf("Cannot shrink volume: the allocation file is too small to describe the volume\n");
		return FALSE;
	}

	/* every block in [newBlocks - 1, oldTotalBlocks - 1) becomes invalid or holds the new
	   alternate volume header, so they all must be free (the old alternate volume header
	   block at oldTotalBlocks - 1 is excluded, since it contains no user data) */
	used = bitmapFindUsedInRange(map, mapSize, newBlocks - 1, oldTotalBlocks - 1);
	free(map);

	if(used >= 0) {
		printf("Cannot shrink volume: block %" PRId64 " is in use\n", used);
		return FALSE;
	}

	/* unreserve the old alternate volume header block; it is being deleted */
	setBlockUsed(volume, oldTotalBlocks - 1, 0);

	/* shrink the allocation file; freed blocks are returned to the system */
	if(volumeHeader->allocationFile.logicalSize > newMapSize) {
		if(!allocate((RawFile*) (volume->allocationFile->data), newMapSize)) {
			printf("Cannot shrink volume: failed to shrink the allocation file\n");
			return FALSE;
		}
	}

	/* mask off garbage bits for blocks that no longer exist in the last partial map byte */
	if((newBlocks % 8) != 0) {
		unsigned char byte = 0;
		off_t offset = (off_t) (newMapSize - 1);
		ASSERT(READ(volume->allocationFile, offset, 1, &byte), "READ");
		byte &= (unsigned char)(0xFF << (8 - (newBlocks % 8)));
		ASSERT(WRITE(volume->allocationFile, offset, 1, &byte), "WRITE");
	}

	/* recount free blocks in the new map */
	{
		uint32_t freeBlocks = 0;
		uint32_t bitBlock = 0;
		uint64_t offset = 0;
		unsigned char chunk[65536];

		while(offset < newMapSize) {
			size_t want = (newMapSize - offset) > sizeof(chunk) ? sizeof(chunk) : (size_t)(newMapSize - offset);
			size_t j;

			ASSERT(READ(volume->allocationFile, offset, want, chunk), "READ");
			for(j = 0; j < want; j++) {
				unsigned char byte = chunk[j];
				int b;
				for(b = 7; b >= 0; b--) {
					uint32_t block = bitBlock + (uint32_t)(7 - b);
					if(block >= newBlocks) {
						break;
					}
					if(!(byte & (1 << b))) {
						freeBlocks++;
					}
				}
				bitBlock += 8;
			}
			offset += want;
		}

		volumeHeader->freeBlocks = freeBlocks;
	}

	/* make sure the allocation hint stays in range */
	if(volumeHeader->nextAllocation >= newBlocks) {
		volumeHeader->nextAllocation = 0;
	}

	/* update the volume geometry */
	volumeHeader->totalBlocks = newBlocks;

	/* reserve the last block for the new alternate volume header */
	setBlockUsed(volume, newBlocks - 1, 1);
	volumeHeader->freeBlocks--;

	/* truncate the backing store */
	if(!TRUNCATE(volume->image, (off_t) newSize)) {
		printf("Warning: volume shrunk, but the backing store could not be truncated\n");
	}

	updateVolume(volume);

	return TRUE;
}

void shrink_hfs(Volume* volume, uint64_t newSize) {
	uint64_t blockSize = volume->volumeHeader->blockSize;
	uint64_t newBlocks64 = newSize / blockSize;

	if((newSize % blockSize) != 0) {
		printf("Warning: size is not a multiple of the block size (%" PRIu64 "), rounding down\n", blockSize);
	}

	if(newBlocks64 < 2) {
		printf("Cannot shrink volume: new size is too small\n");
		return;
	}

	if(newBlocks64 >= volume->volumeHeader->totalBlocks) {
		printf("Cannot shrink volume: new size is not smaller than the current size\n");
		return;
	}

	/* newBlocks fits into 32 bits here because newBlocks64 < totalBlocks (else shrinkVolume rejects it) */
	if(shrinkVolume(volume, (uint32_t) newBlocks64)) {
		printf("shrank volume to: %" PRIu64 "\n", (uint64_t) volume->volumeHeader->totalBlocks * blockSize);
	} else {
		printf("failed to shrink volume\n");
	}
}

void shrink_hfs_min(Volume* volume) {
	HFSPlusVolumeHeader* volumeHeader = volume->volumeHeader;
	uint32_t oldTotalBlocks = volumeHeader->totalBlocks;
	uint64_t mapSize;
	unsigned char* map;
	int64_t highest;
	uint32_t newBlocks;

	map = readAllocationBitmap(volume, &mapSize);
	if(map == NULL) {
		printf("Cannot shrink volume: cannot read the allocation file\n");
		return;
	}

	if(mapSize < ((uint64_t) oldTotalBlocks + 7) / 8) {
		free(map);
		printf("Cannot shrink volume: the allocation file is too small to describe the volume\n");
		return;
	}

	/* the absolute lowest the volume can be shrunk to is one block past the highest
	   used block (excluding the alternate volume header block), plus one block for
	   the new alternate volume header */
	highest = bitmapHighestUsed(map, mapSize, oldTotalBlocks - 1);
	free(map);

	if(highest < 0) {
		printf("Cannot shrink volume: cannot determine the highest used block\n");
		return;
	}

	newBlocks = (uint32_t) highest + 2;

	if(newBlocks >= oldTotalBlocks) {
		printf("Volume is already at its minimum size\n");
		return;
	}

	printf("highest used block: %" PRId64 ", shrinking to %" PRIu32 " blocks\n", highest, newBlocks);

	if(shrinkVolume(volume, newBlocks)) {
		printf("shrank volume to: %" PRIu64 "\n", (uint64_t) volume->volumeHeader->totalBlocks * volume->volumeHeader->blockSize);
	} else {
		printf("failed to shrink volume\n");
	}
}

void removeAllInFolder(HFSCatalogNodeID folderID, Volume* volume, const char* parentName) {
	CatalogRecordList* list;
	CatalogRecordList* theList;
	char fullName[1024];
	char* name;
	char* pathComponent;
	int pathLen;
	char isRoot;
	
	HFSPlusCatalogFolder* folder;
	theList = list = getFolderContents(folderID, volume);
	
	strcpy(fullName, parentName);
	pathComponent = fullName + strlen(fullName);
	
	isRoot = FALSE;
	if(strcmp(fullName, "/") == 0) {
		isRoot = TRUE;
	}
	
	while(list != NULL) {
		name = unicodeToAscii(&list->name);
		if(isRoot && (name[0] == '\0' || strncmp(name, ".HFS+ Private Directory Data", sizeof(".HFS+ Private Directory Data") - 1) == 0)) {
			free(name);
			list = list->next;
			continue;
		}
		
		strcpy(pathComponent, name);
		pathLen = strlen(fullName);
		
		if(list->record->recordType == kHFSPlusFolderRecord) {
			folder = (HFSPlusCatalogFolder*)list->record;
			fullName[pathLen] = '/';
			fullName[pathLen + 1] = '\0';
			removeAllInFolder(folder->folderID, volume, fullName);
		} else {
			printf("%s\n", fullName);
			removeFile(fullName, volume);
		}
		
		free(name);
		list = list->next;
	}
	
	releaseCatalogRecordList(theList);
	
	if(!isRoot) {
		*(pathComponent - 1) = '\0';
		printf("%s\n", fullName);
		removeFile(fullName, volume);
	}
}


void addAllInFolder(HFSCatalogNodeID folderID, Volume* volume, const char* parentName) {
	CatalogRecordList* list;
	CatalogRecordList* theList;
	char cwd[1024];
	char fullName[1024];
	char testBuffer[1024];
	char* pathComponent;
	int pathLen;
	
	char* name;
	
	DIR* dir;
	DIR* tmp;
	
	HFSCatalogNodeID cnid;
	
	struct dirent* ent;
	
	AbstractFile* file;
	HFSPlusCatalogFile* outFile;
	
	strcpy(fullName, parentName);
	pathComponent = fullName + strlen(fullName);
	
	ASSERT(getcwd(cwd, 1024) != NULL, "cannot get current working directory");
	
	theList = list = getFolderContents(folderID, volume);
	
	ASSERT((dir = opendir(cwd)) != NULL, "opendir");
	
	while((ent = readdir(dir)) != NULL) {
		if(ent->d_name[0] == '.' && (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
			continue;
		}
		
		strcpy(pathComponent, ent->d_name);
		pathLen = strlen(fullName);
		
		cnid = 0;
		list = theList;
		while(list != NULL) {
			name = unicodeToAscii(&list->name);
			if(strcmp(name, ent->d_name) == 0) {
				cnid = (list->record->recordType == kHFSPlusFolderRecord) ? (((HFSPlusCatalogFolder*)list->record)->folderID)
				: (((HFSPlusCatalogFile*)list->record)->fileID);
				free(name);
				break;
			}
			free(name);
			list = list->next;
		}
		
		struct stat st;
		ASSERT (lstat(ent->d_name, &st) == 0, "lstat");

		if(S_ISLNK(st.st_mode))
		{
			printf("link: %s\n", fullName);	fflush(stdout);

			io_func* io;
			HFSPlusCatalogFile* record;

			record = (HFSPlusCatalogFile*) getRecordFromPath3(fullName, volume, NULL, NULL, TRUE, FALSE, kHFSRootFolderID);

			if(!record)
			{
				newFile(fullName, volume);
				record = (HFSPlusCatalogFile*) getRecordFromPath(fullName, volume, NULL, NULL);
				if(record)
				{
					record->permissions.fileMode |= S_IFLNK;
					record->userInfo.fileType = kSymLinkFileType;
					record->userInfo.fileCreator = kSymLinkCreator;
					updateCatalog(volume, (HFSPlusCatalogRecord*) record);

					io = openRawFile(record->fileID, &record->dataFork, (HFSPlusCatalogRecord*) record, volume);

					char target[1024 + 1] = {'\0'};
					int rv = readlink(ent->d_name, target, 1024);

					printf("file link: %s %s\n", ent->d_name, target);	fflush(stdout);

					if (rv != -1)
					{
						WRITE(io, 0, strlen(target), (void*) target);
					}
					CLOSE(io);
				}
			}


			free(record);

		}
		else if((tmp = opendir(ent->d_name)) != NULL) {
			closedir(tmp);
			printf("folder: %s\n", fullName); fflush(stdout);
			
			if(cnid == 0) {
				cnid = newFolder(fullName, volume);
			}
			
			fullName[pathLen] = '/';
			fullName[pathLen + 1] = '\0';
			ASSERT(chdir(ent->d_name) == 0, "chdir");
			addAllInFolder(cnid, volume, fullName);
			ASSERT(chdir(cwd) == 0, "chdir");
		} else {
			printf("file: %s\n", fullName);	fflush(stdout);
			if(cnid == 0) {
				cnid = newFile(fullName, volume);
			}
			file = createAbstractFileFromFile(fopen(ent->d_name, "rb"));
			ASSERT(file != NULL, "fopen");
			outFile = (HFSPlusCatalogFile*)getRecordByCNID(cnid, volume);
			writeToHFSFile(outFile, file, volume);
			file->close(file);
			free(outFile);

			/* Copy permissions from the source file */
			struct stat st;
			ASSERT (lstat(ent->d_name, &st) == 0, "lstat");
			chmodFile(fullName, (int)st.st_mode, volume);
			printf("Setting permissions to %06o for %s\n", st.st_mode, fullName);
			
			if(strncmp(fullName, "/Applications/", sizeof("/Applications/") - 1) == 0) {
				testBuffer[0] = '\0';
				strcpy(testBuffer, "/Applications/");
				strcat(testBuffer, ent->d_name);
				strcat(testBuffer, ".app/");
				strcat(testBuffer, ent->d_name);
				if(strcmp(testBuffer, fullName) == 0) {
					if(strcmp(ent->d_name, "Installer") == 0
					|| strcmp(ent->d_name, "BootNeuter") == 0
					) {
						printf("Giving setuid permissions to %s...\n", fullName); fflush(stdout);
						chmodFile(fullName, 04755, volume);
					} else {
						printf("Giving permissions to %s\n", fullName); fflush(stdout);
						chmodFile(fullName, 0755, volume);
					}
				}
			} else if(strncmp(fullName, "/bin/", sizeof("/bin/") - 1) == 0
				|| strncmp(fullName, "/Applications/BootNeuter.app/bin/", sizeof("/Applications/BootNeuter.app/bin/") - 1) == 0
				|| strncmp(fullName, "/sbin/", sizeof("/sbin/") - 1) == 0
				|| strncmp(fullName, "/usr/sbin/", sizeof("/usr/sbin/") - 1) == 0
				|| strncmp(fullName, "/usr/bin/", sizeof("/usr/bin/") - 1) == 0
				|| strncmp(fullName, "/usr/libexec/", sizeof("/usr/libexec/") - 1) == 0
				|| strncmp(fullName, "/usr/local/bin/", sizeof("/usr/local/bin/") - 1) == 0
				|| strncmp(fullName, "/usr/local/sbin/", sizeof("/usr/local/sbin/") - 1) == 0
				|| strncmp(fullName, "/usr/local/libexec/", sizeof("/usr/local/libexec/") - 1) == 0
				) {
				chmodFile(fullName, 0755, volume);
				printf("Giving permissions to %s\n", fullName); fflush(stdout);
			}
		}
	}
	
	closedir(dir);
	
	releaseCatalogRecordList(theList);
}

void extractAllInFolder(HFSCatalogNodeID folderID, Volume* volume) {
	CatalogRecordList* list;
	CatalogRecordList* theList;
	char cwd[1024];
	char* name;
	HFSPlusCatalogFolder* folder;
	HFSPlusCatalogFile* file;
	AbstractFile* outFile;
	struct stat status;
	
	ASSERT(getcwd(cwd, 1024) != NULL, "cannot get current working directory");
	
	theList = list = getFolderContents(folderID, volume);
	
	while(list != NULL) {
		name = unicodeToAscii(&list->name);
		if(strncmp(name, ".HFS+ Private Directory Data", sizeof(".HFS+ Private Directory Data") - 1) == 0 || name[0] == '\0') {
			free(name);
			list = list->next;
			continue;
		}
		
		if(list->record->recordType == kHFSPlusFolderRecord) {
			folder = (HFSPlusCatalogFolder*)list->record;
			printf("folder: %s\n", name);
			if(stat(name, &status) != 0) {
				ASSERT(mkdir(name, 0755) == 0, "mkdir");
			}
			ASSERT(chdir(name) == 0, "chdir");
			extractAllInFolder(folder->folderID, volume);
			ASSERT(chdir(cwd) == 0, "chdir");
		} else if(list->record->recordType == kHFSPlusFileRecord) {
			char *linkName;
			file = (HFSPlusCatalogFile*)list->record;
#ifndef WIN32
			linkName = getLinkName(volume, file);
			if (linkName) {
				printf("link: %s -> %s\n", name, linkName);
				if (symlink(linkName, name)) {
					printf("WARNING: cannot link %s\n", name);
				}
				free(linkName);
			} else
#endif
			{
				printf("file: %s\n", name);
				outFile = createAbstractFileFromFile(fopen(name, "wb"));
				if(outFile != NULL) {
					writeToFile(file, outFile, volume);
					outFile->close(outFile);
				} else {
					printf("WARNING: cannot fopen %s\n", name);
				}
			}
		}
		
		free(name);
		list = list->next;
	}
	releaseCatalogRecordList(theList);
}


void addall_hfs(Volume* volume, const char* dirToMerge, const char* dest) {
	HFSPlusCatalogRecord* record;
	char* name;
	char cwd[1024];
	char initPath[1024];
	int lastCharOfPath;
	
	ASSERT(getcwd(cwd, 1024) != NULL, "cannot get current working directory");
	
	if(chdir(dirToMerge) != 0) {
		printf("Cannot open that directory: %s\n", dirToMerge);
		exit(0);
	}
	
	record = getRecordFromPath(dest, volume, &name, NULL);
	strcpy(initPath, dest);
	lastCharOfPath = strlen(dest) - 1;
	if(dest[lastCharOfPath] != '/') {
		initPath[lastCharOfPath + 1] = '/';
		initPath[lastCharOfPath + 2] = '\0';
	}
	
	if(record != NULL) {
		if(record->recordType == kHFSPlusFolderRecord)
			addAllInFolder(((HFSPlusCatalogFolder*)record)->folderID, volume, initPath);  
		else {
			printf("Not a folder\n");
			exit(0);
		}
	} else {
		printf("No such file or directory\n");
		exit(0);
	}
	
	ASSERT(chdir(cwd) == 0, "chdir");
	free(record);
	
}

int copyAcrossVolumes(Volume* volume1, Volume* volume2, char* path1, char* path2) {
	void* buffer;
	size_t bufferSize;
	AbstractFile* tmpFile;
	int ret;
	
	buffer = malloc(1);
	bufferSize = 0;
	tmpFile = createAbstractFileFromMemoryFile((void**)&buffer, &bufferSize);
	
	if(!silence)
	{
		printf("retrieving... "); fflush(stdout);
	}

	get_hfs(volume1, path1, tmpFile);
	tmpFile->seek(tmpFile, 0);

	if(!silence)
	{
		printf("writing (%ld)... ", (long) tmpFile->getLength(tmpFile)); fflush(stdout);
	}

	ret = add_hfs(volume2, tmpFile, path2);

	if(!silence)
	{
		printf("done\n");
	}
	
	free(buffer);
	
	return ret;
}

void displayFolder(HFSCatalogNodeID folderID, Volume* volume) {
	CatalogRecordList* list;
	CatalogRecordList* theList;
	HFSPlusCatalogFolder* folder;
	HFSPlusCatalogFile* file;
	time_t fileTime;
	struct tm *date;
	HFSPlusDecmpfs* compressData;
	size_t attrSize;
	
	theList = list = getFolderContents(folderID, volume);
	
	while(list != NULL) {
		if(list->record->recordType == kHFSPlusFolderRecord) {
			folder = (HFSPlusCatalogFolder*)list->record;
			printf("%06o ", folder->permissions.fileMode);
			printf("%3d ", folder->permissions.ownerID);
			printf("%3d ", folder->permissions.groupID);
			printf("%12d ", folder->valence);
			fileTime = APPLE_TO_UNIX_TIME(folder->contentModDate);
		} else if(list->record->recordType == kHFSPlusFileRecord) {
			file = (HFSPlusCatalogFile*)list->record;
			printf("%06o ", file->permissions.fileMode);
			printf("%3d ", file->permissions.ownerID);
			printf("%3d ", file->permissions.groupID);
			if(file->permissions.ownerFlags & UF_COMPRESSED) {
				attrSize = getAttribute(volume, file->fileID, "com.apple.decmpfs", (uint8_t**)(&compressData));
				flipHFSPlusDecmpfs(compressData);
				printf("%12" PRId64 " ", compressData->size);
				free(compressData);
			} else {
				printf("%12" PRId64 " ", file->dataFork.logicalSize);
			}
			fileTime = APPLE_TO_UNIX_TIME(file->contentModDate);
		}
			
		date = localtime(&fileTime);
		if(date != NULL) {
			printf("%2d/%2d/%4d %02d:%02d ", date->tm_mon, date->tm_mday, date->tm_year + 1900, date->tm_hour, date->tm_min);
		} else {
			printf("                 ");
		}

		printUnicode(&list->name);
		printf("\n");
		
		list = list->next;
	}
	
	releaseCatalogRecordList(theList);
}

void displayFileLSLine(Volume* volume, HFSPlusCatalogFile* file, const char* name) {
	time_t fileTime;
	struct tm *date;
	HFSPlusDecmpfs* compressData;
	
	printf("%06o ", file->permissions.fileMode);
	printf("%3d ", file->permissions.ownerID);
	printf("%3d ", file->permissions.groupID);

	if(file->permissions.ownerFlags & UF_COMPRESSED) {
		getAttribute(volume, file->fileID, "com.apple.decmpfs", (uint8_t**)(&compressData));
		flipHFSPlusDecmpfs(compressData);
		printf("%12" PRId64 " ", compressData->size);
		free(compressData);
	} else {
		printf("%12" PRId64 " ", file->dataFork.logicalSize);
	}

	fileTime = APPLE_TO_UNIX_TIME(file->contentModDate);
	date = localtime(&fileTime);
	if(date != NULL) {
		printf("%2d/%2d/%4d %2d:%02d ", date->tm_mon, date->tm_mday, date->tm_year + 1900, date->tm_hour, date->tm_min);
	} else {
		printf("                 ");
	}
	printf("%s\n", name);

	XAttrList* next;
	XAttrList* attrs = getAllExtendedAttributes(file->fileID, volume);
	if(attrs != NULL) {
		printf("Extended attributes\n");
		while(attrs != NULL) {
			next = attrs->next;
			printf("\t%s\n", attrs->name);
			free(attrs->name);
			free(attrs);
			attrs = next;
		}	
	}	
}

void hfs_ls(Volume* volume, const char* path) {
	HFSPlusCatalogRecord* record;
	char* name;

	record = getRecordFromPath2(path, volume, &name, NULL, FALSE);
	
	printf("%s: \n", name);
	if(record != NULL) {
		if(record->recordType == kHFSPlusFolderRecord)
			displayFolder(((HFSPlusCatalogFolder*)record)->folderID, volume);  
		else
			displayFileLSLine(volume, (HFSPlusCatalogFile*)record, name);
	} else {
		printf("No such file or directory\n");
	}

	printf("Total filesystem size: %lld, free: %lld\n", ((uint64_t)(volume->volumeHeader->totalBlocks - volume->volumeHeader->freeBlocks)) * volume->volumeHeader->blockSize, ((uint64_t)volume->volumeHeader->freeBlocks) * volume->volumeHeader->blockSize); // cast to uint64_t to be platform independent.
	
	free(record);
}

char *pathCat(const char *path, char *buf) {
	int len;
	while (*path == '/') {
		path++;
	}
	len = strlen(path);
	while (len && path[len - 1] == '/') {
		len--;
	}
	if (len) {
		char *p, *tmp = buf;
		buf = malloc(len + 1 + strlen(tmp) + 1);
		p = buf + len;
		memcpy(buf, path, len);
		*p++ = '/';
		strcpy(p, tmp);
		free(tmp);
	}
	return buf;
}

void printPerm(uint16_t fileMode) {
	uint16_t ifmt = fileMode & 0170000;
	uint16_t spec = fileMode & 0007000;
	uint16_t perm = fileMode & 0000777;

	switch (ifmt) {
		case 0010000: printf("p"); break;
		case 0020000: printf("c"); break;
		case 0040000: printf("d"); break;
		case 0060000: printf("b"); break;
		case 0100000: printf("-"); break;
		case 0120000: printf("l"); break;
		case 0140000: printf("s"); break;
		/*case 0160000: printf("w"); break;*/
		default: printf("?");
	}

	printf("%c%c",  (perm & 0000400) ? 'r' : '-',
			(perm & 0000200) ? 'w' : '-');
	if (spec & 0004000) {
		printf("%c", (perm & 0000100) ? 's' : 'S');
	} else {
		printf("%c", (perm & 0000100) ? 'x' : '-');
	}

	printf("%c%c",  (perm & 0000040) ? 'r' : '-',
			(perm & 0000020) ? 'w' : '-');
	if (spec & 0002000) {
		printf("%c", (perm & 0000010) ? 's' : 'S');
	} else {
		printf("%c", (perm & 0000010) ? 'x' : '-');
	}

	printf("%c%c",  (perm & 0000004) ? 'r' : '-',
			(perm & 0000002) ? 'w' : '-');
	if (spec & 0001000) {
		printf("%c", (perm & 0000001) ? 't' : 'T');
	} else {
		printf("%c", (perm & 0000001) ? 'x' : '-');
	}

	printf(" ");
}

void displayFileLSLine2(Volume* volume, HFSPlusCatalogFile* file, const char *name) {
	time_t fileTime;
	struct tm *date;
	HFSPlusDecmpfs* compressData;
	size_t attrSize;
	char *linkName;

	printPerm(file->permissions.fileMode);
	printf("%5d ", 1);
	printf("%3d ", file->permissions.ownerID);
	printf("%3d ", file->permissions.groupID);
	if(file->permissions.ownerFlags & UF_COMPRESSED) {
		attrSize = getAttribute(volume, file->fileID, "com.apple.decmpfs", (uint8_t**)(&compressData));
		flipHFSPlusDecmpfs(compressData);
		printf("%12" PRId64 " ", compressData->size);
		free(compressData);
	} else {
		printf("%12" PRId64 " ", file->dataFork.logicalSize);
	}
	fileTime = APPLE_TO_UNIX_TIME(file->contentModDate);
	linkName = getLinkName(volume, file);

	date = localtime(&fileTime);
	if(date != NULL) {
		printf("%02d-%02d-%04d %02d:%02d ", date->tm_mon, date->tm_mday, date->tm_year + 1900, date->tm_hour, date->tm_min);
	} else {
		printf("                 ");
	}

	printf("%s", name);
	if (linkName) {
		printf(" -> %s", linkName);
		free(linkName);
	}
	printf("\n");
}

void displayFolder2(HFSCatalogNodeID folderID, Volume* volume, const char *path, int recursive) {
	CatalogRecordList* list;
	CatalogRecordList* theList;
	HFSPlusCatalogFolder* folder;
	HFSPlusCatalogFile* file;
	time_t fileTime;
	struct tm *date;
	
	theList = list = getFolderContents(folderID, volume);
	
	while(list != NULL) {
		char *buf = unicodeToAscii(&list->name);
		if (*buf) {
			buf = pathCat(path, buf);
			if(list->record->recordType == kHFSPlusFolderRecord) {
				folder = (HFSPlusCatalogFolder*)list->record;
				printPerm(folder->permissions.fileMode);
				printf("%5d ", folder->valence + 1);
				printf("%3d ", folder->permissions.ownerID);
				printf("%3d ", folder->permissions.groupID);
				printf("%12d ", 0);
				fileTime = APPLE_TO_UNIX_TIME(folder->contentModDate);

				date = localtime(&fileTime);
				if(date != NULL) {
					printf("%02d-%02d-%04d %02d:%02d ", date->tm_mon, date->tm_mday, date->tm_year + 1900, date->tm_hour, date->tm_min);
				} else {
					printf("                 ");
				}

				printf("%s\n", buf);

				if(recursive && list->record->recordType == kHFSPlusFolderRecord) {
					displayFolder2(folder->folderID, volume, buf, recursive);
				}
			} else if(list->record->recordType == kHFSPlusFileRecord) {
				displayFileLSLine2(volume, (HFSPlusCatalogFile*)list->record, buf);
			}
		}
		free(buf);
		list = list->next;
	}
	
	releaseCatalogRecordList(theList);
}

void hfs_list(Volume* volume, const char *path, int recursive) {
	HFSPlusCatalogRecord* record;
	char* name;

	record = getRecordFromPath2(path, volume, &name, NULL, FALSE);

	if(record != NULL) {
		if(record->recordType == kHFSPlusFolderRecord)
			displayFolder2(((HFSPlusCatalogFolder*)record)->folderID, volume, path, recursive);
		else {
			while (*path == '/') {
				path++;
			}
			displayFileLSLine2(volume, (HFSPlusCatalogFile*)record, path);
		}
	} else {
		fprintf(stderr, "%s: No such file or directory\n", path);
	}
	free(record);
}

void hfs_untar(Volume* volume, AbstractFile* tarFile) {
	size_t tarSize = tarFile->getLength(tarFile);
	size_t curRecord = 0;
	char block[512];
	char *longName = NULL;
	char *longLink = NULL;
	char *paxPath = NULL;      /* POSIX.1-2001 pax extended header keywords (apply to the next entry) */
	char *paxLinkPath = NULL;

	while(curRecord < tarSize) {
		tarFile->seek(tarFile, curRecord);
		tarFile->read(tarFile, block, 512);

		uint32_t mode = 0;
		char* fileName = NULL;
		const char* target = NULL;
		uint32_t type = 0;
		uint32_t size;
		uint32_t uid;
		uint32_t gid;

		sscanf(&block[100], "%o", &mode);
		fileName = &block[0];
		sscanf(&block[156], "%o", &type);
		target = &block[157];
		sscanf(&block[124], "%o", &size);
		sscanf(&block[108], "%o", &uid);
		sscanf(&block[116], "%o", &gid);

		if (block[156] == 'L') {
			longName = malloc(size);
			tarFile->seek(tarFile, curRecord + 512);
			tarFile->read(tarFile, longName, size);
			goto loop;
		}
		if (block[156] == 'K') {
			longLink = malloc(size);
			tarFile->seek(tarFile, curRecord + 512);
			tarFile->read(tarFile, longLink, size);
			goto loop;
		}
		if (block[156] == 'x' || block[156] == 'g') {
			/* POSIX.1-2001 extended header ("mhjvzp"): lines of "<decimal-length> <keyword>=<value>\n"
			   where the length counts the entire line; we honour path= and linkpath= */
			char* data = (char*) malloc((size_t) size + 1);
			size_t i = 0;

			tarFile->seek(tarFile, curRecord + 512);
			tarFile->read(tarFile, data, size);
			data[size] = '\0';

			while(i < size) {
				unsigned long recLen = 0;
				size_t recStart, recEnd;
				char* eq;
				char* nl;
				char* value;

				recStart = i;
				while(i < size && data[i] != ' ') {
					if(data[i] < '0' || data[i] > '9') { recLen = 0; break; }
					recLen = (recLen * 10) + (unsigned long)(data[i] - '0');
					i++;
				}

				if(i >= size || data[i] != ' ' || recLen == 0) {
					break;
				}

				i++; /* skip the space */
				recEnd = recStart + recLen;

				if(recEnd > size) {
					break;
				}

				eq = (char*) memchr(data + i, '=', recEnd - i);

				if(eq != NULL) {
					*eq = '\0';
					value = eq + 1;
					nl = (char*) memchr(value, '\n', recEnd - (size_t)(value - data));

					if(nl != NULL) {
						*nl = '\0';
					}

					if(strcmp(data + i, "path") == 0) {
						free(paxPath);
						paxPath = strdup(value);
					} else if(strcmp(data + i, "linkpath") == 0) {
						free(paxLinkPath);
						paxLinkPath = strdup(value);
					}

					*eq = '=';
				}

				i = recEnd;
			}

			free(data);
			goto loop;
		}

		if (longName) {
			fileName = longName;
		} else if (strncmp(&block[257], "ustar", 5) == 0 && block[345] != '\0') {
			/* ustar splits long paths between the prefix field (offset 345) and the name field */
			size_t prefixLen = 0;
			size_t nameLen = 0;
			while (prefixLen < 155 && block[345 + prefixLen] != '\0') prefixLen++;
			while (nameLen < 100 && block[0 + nameLen] != '\0') nameLen++;
			{
				char* full = (char*) malloc(prefixLen + 1 + nameLen + 1);
				memcpy(full, &block[345], prefixLen);
				full[prefixLen] = '/';
				memcpy(full + prefixLen + 1, &block[0], nameLen);
				full[prefixLen + 1 + nameLen] = '\0';
				longName = full; /* freed at the end of the record like any other long name */
				fileName = longName;
			}
		} else {
			/* bound the name field in case it is exactly 100 bytes with no terminating NUL */
			size_t nameLen = 0;
			while(nameLen < 100 && block[nameLen] != '\0') nameLen++;
			if(nameLen == 100) {
				longName = (char*) malloc(101);
				memcpy(longName, block, 100);
				longName[100] = '\0';
				fileName = longName;
			}
		}

		if(target == &block[157]) {
			/* bound the linkname field in the same way */
			size_t linkLen = 0;
			while(linkLen < 100 && block[157 + linkLen] != '\0') linkLen++;
			if(linkLen == 100) {
				longLink = (char*) malloc(101);
				memcpy(longLink, &block[157], 100);
				longLink[100] = '\0';
			}
		}
		if (longLink)
			target = longLink;

		/* pax extended headers override the ustar fields for the entry they precede */
		if(paxPath) {
			free(longName);
			longName = paxPath;
			paxPath = NULL;
			fileName = longName;
		}
		if(paxLinkPath) {
			free(longLink);
			longLink = paxLinkPath;
			paxLinkPath = NULL;
			target = longLink;
		}

		if(fileName[0] == '\0')
			break;

		if(fileName[0] == '.' && fileName[1] == '/') {
			fileName += 2;
		}

		if(fileName[0] == '\0')
			goto loop;

		if(fileName[strlen(fileName) - 1] == '/')
			fileName[strlen(fileName) - 1] = '\0';

		HFSPlusCatalogRecord* record = getRecordFromPath3(fileName, volume, NULL, NULL, TRUE, FALSE, kHFSRootFolderID);
		if(record) {
			if(record->recordType == kHFSPlusFolderRecord || type == 5) {
				if(!silence)
					printf("ignoring %s, type = %d\n", fileName, type);
				free(record);
				goto loop;
			} else {
				printf("replacing %s\n", fileName);
				free(record);
				removeFile(fileName, volume);
			}
		}

		if(type == 0) {
			if(!silence)
				printf("file: %s (%04o), size = %d\n", fileName, mode, size);
			void* buffer = malloc(size);
			tarFile->seek(tarFile, curRecord + 512);
			tarFile->read(tarFile, buffer, size);
			AbstractFile* inFile = createAbstractFileFromMemory(&buffer, size);
			add_hfs(volume, inFile, fileName);
			free(buffer);
		} else if(type == 5) {
			if(!silence)
				printf("directory: %s (%04o)\n", fileName, mode);
			newFolder(fileName, volume);
		} else if(type == 2) {
			if(!silence)
				printf("symlink: %s (%04o) -> %s\n", fileName, mode, target);
			makeSymlink(fileName, target, volume);
		}

		chmodFile(fileName, mode, volume);
		chownFile(fileName, uid, gid, volume);
		free(longName);
		longName = NULL;
		free(longLink);
		longLink = NULL;

loop:

		curRecord = (curRecord + 512) + ((size + 511) / 512 * 512);
	}

	free(paxPath);
	free(paxLinkPath);
}


/* ---------- tar writer (ustar with GNU long name/long link extensions) ---------- */

static void tarWriteOctal(unsigned char* field, size_t fieldSize, uint64_t value) {
	/* zero-padded octal with a trailing NUL, like classic tar writers */
	snprintf((char*) field, fieldSize, "%0*llo", (int) fieldSize - 1, (unsigned long long) value);
}

static void tarWritePadding(AbstractFile* outFile, uint64_t size) {
	static const unsigned char zeros[512];
	uint64_t pad = (512 - (size % 512)) % 512;

	if(pad > 0) {
		outFile->write(outFile, (void*) zeros, (size_t) pad);
	}
}

static uint64_t tarSumHeader(const unsigned char* header) {
	uint64_t sum = 0;
	int i;

	for(i = 0; i < 512; i++) {
		if(i >= 148 && i < 156) {
			sum += 0x20; /* the checksum field is treated as spaces */
		} else {
			sum += header[i];
		}
	}

	return sum;
}

static void tarFinishHeader(unsigned char* header) {
	uint64_t sum = tarSumHeader(header);

	memset(header + 148, 0x20, 8);
	/* 6 octal digits + a space + NUL terminator, exactly like GNU tar */
	snprintf((char*) header + 148, 8, "%06llo ", (unsigned long long) sum);
}

/* the GNU long name/long link pseudo-entry used when a path (or link target)
   is too long for the ustar name/prefix fields */
static void tarWriteLongEntry(AbstractFile* outFile, const char* name, char typeflag) {
	unsigned char header[512];
	uint64_t size = strlen(name) + 1; /* include the trailing NUL */

	memset(header, 0, sizeof(header));
	strcpy((char*) header, "././@LongLink");
	tarWriteOctal(header + 100, 8, 0644);
	tarWriteOctal(header + 108, 8, 0);
	tarWriteOctal(header + 116, 8, 0);
	tarWriteOctal(header + 124, 12, size);
	tarWriteOctal(header + 136, 12, 0);
	memset(header + 148, 0x20, 8);
	header[156] = (unsigned char) typeflag;
	memcpy(header + 257, "ustar\0", 6);
	memcpy(header + 263, "00", 2);
	tarFinishHeader(header);

	outFile->write(outFile, header, sizeof(header));
	outFile->write(outFile, name, (size_t) size);
	tarWritePadding(outFile, size);
}

static void tarWriteHeader(AbstractFile* outFile, const char* name, const char* linkname,
                           uint32_t mode, uint32_t uid, uint32_t gid, uint64_t size,
                           uint32_t appleTime, char typeflag) {
	unsigned char header[512];
	unsigned char shortName[101];
	size_t nameLen = strlen(name);

	memset(header, 0, sizeof(header));
	memset(shortName, 0, sizeof(shortName));

	if(nameLen <= 100) {
		memcpy(shortName, name, nameLen);
	} else {
		/* try a ustar prefix/name split at a '/' */
		int foundSplit = 0;
		const char* p;

		p = name + nameLen;
		while(p > name + 1) {
			p--;
			if(*p == '/') {
				size_t prefixLen = (size_t)(p - name);
				size_t restLen = nameLen - prefixLen - 1;
				if(prefixLen > 0 && prefixLen <= 155 && restLen > 0 && restLen <= 100) {
					memcpy(header + 345, name, prefixLen); /* prefix field at offset 345 */
					memcpy(shortName, p + 1, restLen);
					foundSplit = 1;
					break;
				}
			}
		}

		if(!foundSplit) {
			/* no ustar split possible; use a GNU long name entry */
			tarWriteLongEntry(outFile, name, 'L');
			memcpy(shortName, name, 100);
		}
	}

	if(linkname != NULL && strlen(linkname) > 100) {
		tarWriteLongEntry(outFile, linkname, 'K');
	}

	memcpy(header, shortName, 100);
	tarWriteOctal(header + 100, 8, mode);
	tarWriteOctal(header + 108, 8, uid);
	tarWriteOctal(header + 116, 8, gid);
	tarWriteOctal(header + 124, 12, size);
	tarWriteOctal(header + 136, 12, APPLE_TO_UNIX_TIME(appleTime));
	memset(header + 148, 0x20, 8);
	header[156] = (unsigned char) typeflag;
	if(linkname != NULL) {
		strncpy((char*) header + 157, linkname, 100);
	}
	memcpy(header + 257, "ustar\0", 6);
	memcpy(header + 263, "00", 2);
	tarFinishHeader(header);

	outFile->write(outFile, header, sizeof(header));
}

static uint64_t tarFileDataSize(Volume* volume, HFSPlusCatalogFile* file) {
	if(file->permissions.ownerFlags & UF_COMPRESSED) {
		io_func* io = openHFSPlusCompressed(volume, file);
		if(io != NULL) {
			uint64_t size = ((HFSPlusCompressed*) io->data)->decmpfs->size;
			CLOSE(io);
			return size;
		}
	}

	return file->dataFork.logicalSize;
}

static void hfs_tar_record(Volume* volume, AbstractFile* outFile, HFSPlusCatalogRecord* record, const char* relativePath, unsigned int* count, uint64_t* totalBytes);

static void hfs_tar_folder(Volume* volume, AbstractFile* outFile, HFSCatalogNodeID folderID, const char* relativePath, unsigned int* count, uint64_t* totalBytes) {
	CatalogRecordList* theList;
	CatalogRecordList* list;
	char* name;

	theList = list = getFolderContents(folderID, volume);

	while(list != NULL) {
		name = unicodeToAscii(&list->name);
		if(name[0] == '\0' || strncmp(name, ".HFS+ Private Directory Data", sizeof(".HFS+ Private Directory Data") - 1) == 0) {
			free(name);
			list = list->next;
			continue;
		}

		{
			char childRel[2048];
			int ok;

			if(relativePath[0] == '\0') {
				ok = snprintf(childRel, sizeof(childRel), "%s", name) < (int) sizeof(childRel);
			} else {
				ok = snprintf(childRel, sizeof(childRel), "%s/%s", relativePath, name) < (int) sizeof(childRel);
			}

			if(ok) {
				hfs_tar_record(volume, outFile, list->record, childRel, count, totalBytes);
			} else {
				printf("Warning: path too long, skipping: %s\n", name);
			}
		}

		free(name);
		list = list->next;
	}

	releaseCatalogRecordList(theList);
}

static void hfs_tar_record(Volume* volume, AbstractFile* outFile, HFSPlusCatalogRecord* record, const char* relativePath, unsigned int* count, uint64_t* totalBytes) {
	if(record->recordType == kHFSPlusFolderRecord) {
		HFSPlusCatalogFolder* folder = (HFSPlusCatalogFolder*) record;
		char dirName[2048];

		snprintf(dirName, sizeof(dirName), "%s/", relativePath);
		tarWriteHeader(outFile, dirName, NULL, folder->permissions.fileMode & 07777,
					   folder->permissions.ownerID, folder->permissions.groupID,
					   0, folder->contentModDate, '5');
		(*count)++;
		if(!silence) {
			printf("directory: %s (%04o)\n", dirName, folder->permissions.fileMode & 07777);
		}

		hfs_tar_folder(volume, outFile, folder->folderID, relativePath, count, totalBytes);
	} else if(record->recordType == kHFSPlusFileRecord) {
		HFSPlusCatalogFile* file = (HFSPlusCatalogFile*) record;
		char* linkName = NULL;

		if((file->permissions.fileMode & 0170000) == 0120000) {
			linkName = getLinkName(volume, file);
		}

		if(linkName != NULL) {
			tarWriteHeader(outFile, relativePath, linkName, file->permissions.fileMode & 07777,
						   file->permissions.ownerID, file->permissions.groupID,
						   0, file->contentModDate, '2');
			(*count)++;
			if(!silence) {
				printf("symlink: %s -> %s\n", relativePath, linkName);
			}
			free(linkName);
		} else {
			uint64_t size = tarFileDataSize(volume, file);
			tarWriteHeader(outFile, relativePath, NULL, file->permissions.fileMode & 07777,
						   file->permissions.ownerID, file->permissions.groupID,
						   size, file->contentModDate, '0');
			writeToFile(file, outFile, volume);
			tarWritePadding(outFile, size);
			(*count)++;
			*totalBytes += size;
			if(!silence) {
				printf("file: %s (%04o), size = %" PRIu64 "\n", relativePath, file->permissions.fileMode & 07777, size);
			}
		}
	}
}

void hfs_tar(Volume* volume, AbstractFile* outFile, const char* path) {
	unsigned int count = 0;
	uint64_t totalBytes = 0;
	char normalizedPath[2048];
	char relativePath[2048];
	static const unsigned char endBlock[1024];
	size_t length;

	/* normalize: strip trailing slashes (but keep "/") */
	length = strlen(path);
	if(length >= sizeof(normalizedPath)) {
		length = sizeof(normalizedPath) - 1;
	}
	memcpy(normalizedPath, path, length);
	normalizedPath[length] = '\0';
	while(length > 1 && normalizedPath[length - 1] == '/') {
		normalizedPath[--length] = '\0';
	}

	if(strcmp(normalizedPath, "/") == 0) {
		/* archive the contents of the root volume without emitting a root entry */
		HFSPlusCatalogRecord* record = getRecordFromPath("/", volume, NULL, NULL);
		if(record == NULL) {
			printf("No such file or directory: %s\n", normalizedPath);
			outFile->write(outFile, (void*) endBlock, sizeof(endBlock));
			return;
		}
		hfs_tar_folder(volume, outFile, ((HFSPlusCatalogFolder*) record)->folderID, "", &count, &totalBytes);
		free(record);
	} else {
		const char* base = strrchr(normalizedPath, '/');
		HFSPlusCatalogRecord* record;

		base = base ? base + 1 : normalizedPath;
		snprintf(relativePath, sizeof(relativePath), "%s", base);

		record = getRecordFromPath(normalizedPath, volume, NULL, NULL);
		if(record == NULL) {
			printf("No such file or directory: %s\n", normalizedPath);
			outFile->write(outFile, (void*) endBlock, sizeof(endBlock));
			return;
		}
		hfs_tar_record(volume, outFile, record, relativePath, &count, &totalBytes);
		free(record);
	}

	/* an end of archive marker: two empty 512-byte blocks */
	outFile->write(outFile, (void*) endBlock, sizeof(endBlock));

	if(!silence) {
		printf("tarred %u files/folders (%" PRIu64 " bytes) to archive\n", count, totalBytes);
	}
}
