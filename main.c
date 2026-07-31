#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <conio.h>

typedef struct {
    unsigned int signature;
    const char* extension;
    int offsetCorrection;
    unsigned long long maxSize;
} FileSignature;

FileSignature signatures[] = {
    { 0x70797466, "mp4", -4, 30ULL * 1024 * 1024 * 1024 }, 
    { 0x46464952, "avi",  0, 30ULL * 1024 * 1024 * 1024 }, 
    { 0xA3DF451A, "mkv",  0, 30ULL * 1024 * 1024 * 1024 }, 
    { 0xE0FFD8FF, "jpg",  0, 15ULL * 1024 * 1024 },       
    { 0xE1FFD8FF, "jpg",  0, 25ULL * 1024 * 1024 },       
    { 0x474E5089, "png",  0, 15ULL * 1024 * 1024 },       
    { 0x04034B50, "zip",  0, 500ULL * 1024 * 1024 },      
    { 0x46445025, "pdf",  0, 50ULL * 1024 * 1024 }        
};
int sigCount = sizeof(signatures) / sizeof(FileSignature);

BOOL ReadDiskSafe(HANDLE hDisk, unsigned long long offset, void* buffer, DWORD size) {
    DWORD SECTOR = 4096;
    unsigned long long alignedOffset = (offset / SECTOR) * SECTOR;
    DWORD diff = (DWORD)(offset - alignedOffset);
    DWORD readSize = ((diff + size + SECTOR - 1) / SECTOR) * SECTOR;

    unsigned char temp[8192]; 
    if (readSize > sizeof(temp)) return FALSE; 

    LARGE_INTEGER pos;
    pos.QuadPart = alignedOffset;
    if (!SetFilePointerEx(hDisk, pos, NULL, FILE_BEGIN)) return FALSE;

    DWORD br;
    if (ReadFile(hDisk, temp, readSize, &br, NULL) && br >= diff + size) {
        memcpy(buffer, temp + diff, size);
        return TRUE;
    }
    return FALSE;
}

unsigned long long ParseAviSize(HANDLE hDisk, unsigned long long absoluteStart) {
    unsigned char header[8];
    if (ReadDiskSafe(hDisk, absoluteStart, header, 8)) {
        unsigned int reportedSize = *(unsigned int*)(header + 4);
        return (unsigned long long)reportedSize + 8;
    }
    return 0;
}

unsigned long long ParseMp4Size(HANDLE hDisk, unsigned long long absoluteStart) {
    unsigned char header[16];
    unsigned long long currentOffset = absoluteStart;
    unsigned long long totalSize = 0;

    while (1) {
        if (!ReadDiskSafe(hDisk, currentOffset, header, 16)) break;

        unsigned int size32 = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
        unsigned long long boxSize = size32;

        if (size32 == 1) { 
            boxSize = 
                ((unsigned long long)header[8]  << 56) | ((unsigned long long)header[9]  << 48) |
                ((unsigned long long)header[10] << 40) | ((unsigned long long)header[11] << 32) |
                ((unsigned long long)header[12] << 24) | ((unsigned long long)header[13] << 16) |
                ((unsigned long long)header[14] << 8)  |  (unsigned long long)header[15];
        } else if (size32 == 0) {
            return 0; 
        }

        if (boxSize < 8) break; 

        int isAscii = 1;
        for (int i = 4; i < 8; i++) {
            if (header[i] < 0x20 || header[i] > 0x7E) isAscii = 0;
        }
        if (!isAscii) break; 

        totalSize += boxSize;
        currentOffset += boxSize;

        if (totalSize > 50ULL * 1024 * 1024 * 1024) return 0; 
    }
    return totalSize;
}

int ValidateSignature(unsigned char* ptr, unsigned long long remainingBytes, const char* ext) {
    if (remainingBytes < 16) return 1; 

    if (strcmp(ext, "mp4") == 0) {
        unsigned char b1 = ptr[4], b2 = ptr[5], b3 = ptr[6], b4 = ptr[7];
        int c1 = ((b1 >= 'a' && b1 <= 'z') || (b1 >= 'A' && b1 <= 'Z') || (b1 >= '0' && b1 <= '9') || b1 == ' ');
        int c2 = ((b2 >= 'a' && b2 <= 'z') || (b2 >= 'A' && b2 <= 'Z') || (b2 >= '0' && b2 <= '9') || b2 == ' ');
        int c3 = ((b3 >= 'a' && b3 <= 'z') || (b3 >= 'A' && b3 <= 'Z') || (b3 >= '0' && b3 <= '9') || b3 == ' ');
        int c4 = ((b4 >= 'a' && b4 <= 'z') || (b4 >= 'A' && b4 <= 'Z') || (b4 >= '0' && b4 <= '9') || b4 == ' ');
        return (c1 && c2 && c3 && c4) ? 1 : 0;
    }
    if (strcmp(ext, "mkv") == 0) {
        for (int i = 4; i < 64 && i < remainingBytes - 8; i++) {
            if (ptr[i] == 'm' && ptr[i+1] == 'a' && ptr[i+2] == 't' && ptr[i+3] == 'r') return 1;
            if (ptr[i] == 'w' && ptr[i+1] == 'e' && ptr[i+2] == 'b' && ptr[i+3] == 'm') return 1;
        }
        return 0; 
    }
    if (strcmp(ext, "jpg") == 0) {
        if (ptr[6] == 'J' && ptr[7] == 'F' && ptr[8] == 'I' && ptr[9] == 'F') return 1;
        if (ptr[6] == 'E' && ptr[7] == 'x' && ptr[8] == 'i' && ptr[9] == 'f') return 1;
        return 0;
    }
    return 1;
}

long long FindValidSignatureFast(unsigned char* buffer, unsigned long long size, int sigIndex) {
    int correction = signatures[sigIndex].offsetCorrection; 
    
    for (unsigned long long sector = 0; sector + 512 <= size; sector += 512) {
        long long checkPos = sector - correction;
        
        if (checkPos >= 0 && checkPos + 64 <= size) {
            unsigned int val = *(unsigned int*)(buffer + checkPos);
            
            if (val == signatures[sigIndex].signature) {
                if (ValidateSignature(buffer + checkPos, size - checkPos, signatures[sigIndex].extension)) {
                    return sector; 
                }
            }
        }
    }
    return -1;
}

long long FindTrailer(unsigned char* buffer, DWORD size, const char* ext) {
    if (strcmp(ext, "pdf") == 0) {
        long long lastPos = -1;
        for (DWORD i = 0; i + 5 <= size; i++) {
            if (buffer[i] == '%' && buffer[i+1] == '%' && buffer[i+2] == 'E' && 
                buffer[i+3] == 'O' && buffer[i+4] == 'F') {
                lastPos = i + 5; 
            }
        }
        return lastPos;
    }
    if (strcmp(ext, "zip") == 0) {
        for (DWORD i = 0; i + 22 <= size; i++) {
            if (buffer[i] == 0x50 && buffer[i+1] == 0x4B && 
                buffer[i+2] == 0x05 && buffer[i+3] == 0x06) {
                return i + 22; 
            }
        }
    }
    return -1;
}

unsigned long long ExtractFile(HANDLE hDisk, unsigned long long absoluteOffset, const char* ext, int fileId, unsigned long long maxSize) {
    char filename[256];
    sprintf_s(filename, sizeof(filename), "recovered_%d.%s", fileId, ext);

    unsigned long long exactSize = 0;
    int useBlade = 1; 

    if (strcmp(ext, "mp4") == 0) exactSize = ParseMp4Size(hDisk, absoluteOffset);
    else if (strcmp(ext, "avi") == 0) exactSize = ParseAviSize(hDisk, absoluteOffset);

    unsigned long long bytesToRead = maxSize;

    if (exactSize > 0) {
        printf("\n[>>>] Выгружаем %s (Точный размер: %.2f МБ)...\n", filename, exactSize / (1024.0 * 1024.0));
        printf("    [i] Защита от фрагментации (Бронированный режим).\n");
        bytesToRead = exactSize;
        useBlade = 0; 
    } else {
        printf("\n[>>>] Выгружаем %s (Ожидание обрезки)...\n", filename);
    }
    fflush(stdout); 

    HANDLE hOutFile = CreateFileA(filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOutFile == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER currentPos, targetPos;
    currentPos.QuadPart = 0;
    SetFilePointerEx(hDisk, currentPos, &currentPos, FILE_CURRENT);

    targetPos.QuadPart = absoluteOffset;
    SetFilePointerEx(hDisk, targetPos, NULL, FILE_BEGIN);

    unsigned long long totalWritten = 0;
    DWORD bufferSize = 32 * 1024 * 1024; 
    unsigned char* copyBuffer = (unsigned char*)VirtualAlloc(NULL, bufferSize, MEM_COMMIT, PAGE_READWRITE);
    int isFirstBlock = 1;

    while (bytesToRead > 0) {
        DWORD attemptRead = bufferSize; 
        DWORD bytesRead = 0, bytesWritten = 0;

        if (ReadFile(hDisk, copyBuffer, attemptRead, &bytesRead, NULL) && bytesRead > 0) {
            
            DWORD validDataLen = (bytesRead > bytesToRead) ? (DWORD)bytesToRead : bytesRead;
            
            DWORD startCheckOffset = isFirstBlock ? 512 : 0; 
            long long bestAlienSector = -1;
            int bestAlienIndex = -1;

            if (strcmp(ext, "zip") == 0 || strcmp(ext, "pdf") == 0) {
                long long trailerPos = FindTrailer(copyBuffer + startCheckOffset, validDataLen - startCheckOffset, ext);
                if (trailerPos != -1) {
                    trailerPos += startCheckOffset;
                    long long safeCut = trailerPos + 512;
                    if (safeCut > bytesRead) safeCut = bytesRead;
                    
                    printf("\r    [!] Найден истинный конец файла (%s).            \n", ext);
                    validDataLen = (DWORD)safeCut;
                    bytesToRead = validDataLen; 
                    useBlade = 0; 
                }
            }

            if (useBlade && validDataLen > startCheckOffset) {
                for (int i = 0; i < sigCount; i++) {

                    if (strcmp(ext, "zip") == 0 || strcmp(ext, "pdf") == 0) {
                        if (strcmp(signatures[i].extension, "jpg") == 0 || 
                            strcmp(signatures[i].extension, "png") == 0 ||
                            strcmp(signatures[i].extension, "pdf") == 0 ||
                            strcmp(signatures[i].extension, "zip") == 0) {
                            continue; 
                        }
                    }

                    long long alienSector = FindValidSignatureFast(
                        copyBuffer + startCheckOffset, 
                        validDataLen - startCheckOffset, 
                        i
                    );

                    if (alienSector != -1) {
                        if (bestAlienSector == -1 || alienSector < bestAlienSector) {
                            bestAlienSector = alienSector;
                            bestAlienIndex = i;
                        }
                    }
                }
            }

            if (bestAlienSector != -1) {
                long long cutPos = startCheckOffset + bestAlienSector;
                unsigned long long alienAbsolute = absoluteOffset + totalWritten + cutPos;
                
                printf("\r    [!] Файл обрезан на байте %llu (Начало %s)      \n", alienAbsolute, signatures[bestAlienIndex].extension);
                
                validDataLen = (DWORD)cutPos;
                bytesToRead = validDataLen; 
            }
            
            isFirstBlock = 0; 
            WriteFile(hOutFile, copyBuffer, validDataLen, &bytesWritten, NULL);
            totalWritten += bytesWritten;
            bytesToRead -= validDataLen;

            if (totalWritten % (32 * 1024 * 1024) == 0 || bytesToRead == 0) {
                printf("\r    [~] Сохранено: %llu МБ...", totalWritten / (1024 * 1024));
                fflush(stdout);
            }

        } else {
            break; 
        }
    }

    VirtualFree(copyBuffer, 0, MEM_RELEASE);
    CloseHandle(hOutFile);

    SetFilePointerEx(hDisk, currentPos, NULL, FILE_BEGIN);
    printf("\r[+] Файл %s успешно сохранен! Итоговый размер: %.2f МБ         \n\n", filename, totalWritten / (1024.0 * 1024.0));
    fflush(stdout);
    return totalWritten;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    const char* drivePath = "\\\\.\\PhysicalDrive1"; 
    const unsigned long long CHUNK_SIZE = 32 * 1024 * 1024; 
    
    unsigned char* buffer = (unsigned char*)VirtualAlloc(NULL, CHUNK_SIZE, MEM_COMMIT, PAGE_READWRITE);
    
    HANDLE hDevice = CreateFileA(drivePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        printf("[-] Не удалось открыть диск.\n");
        return 1;
    }

    unsigned long long totalDiskSize = 0;
    GET_LENGTH_INFORMATION gli;
    DWORD bytesReturned;
    if (DeviceIoControl(hDevice, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &gli, sizeof(gli), &bytesReturned, NULL)) {
        totalDiskSize = gli.Length.QuadPart;
        printf("[*] Диск открыт. Общий объем: %.2f ГБ\n", totalDiskSize / (1024.0 * 1024.0 * 1024.0));
    }

    printf("[*] Начинаем сканирование (Секторный шаг, буфер 32 МБ)...\n\n");

    unsigned long long globalOffset = 0;
    DWORD bytesRead = 0;
    int recoveredCount = 0;
    unsigned int skipBytesInNextChunk = 0; 

    while (1) {
        if (!ReadFile(hDevice, buffer, CHUNK_SIZE, &bytesRead, NULL)) break;
        if (bytesRead == 0) break; 

        long long localSearchOffset = skipBytesInNextChunk; 
        skipBytesInNextChunk = 0; 
        int fileFoundInChunk = 0;

        while (localSearchOffset < bytesRead) {
            long long bestFoundSector = -1;
            int bestSigIndex = -1;

            for (int i = 0; i < sigCount; i++) {
                long long foundSector = FindValidSignatureFast(
                    buffer + localSearchOffset, 
                    bytesRead - localSearchOffset, 
                    i
                );

                if (foundSector != -1) {
                    if (bestFoundSector == -1 || foundSector < bestFoundSector) {
                        bestFoundSector = foundSector;
                        bestSigIndex = i;
                    }
                }
            }

            if (bestFoundSector != -1) {
                recoveredCount++;
                fileFoundInChunk = 1;
                
                long long actualFileStart = (long long)globalOffset + localSearchOffset + bestFoundSector;

                printf("\r                                                                      \r"); 
                printf("[!] Найдена сигнатура %s на отметке %llu байт.\n", signatures[bestSigIndex].extension, actualFileStart);
                fflush(stdout);

                unsigned long long extractedSize = ExtractFile(hDevice, actualFileStart, signatures[bestSigIndex].extension, recoveredCount, signatures[bestSigIndex].maxSize);

                unsigned long long exactPos = actualFileStart + extractedSize;
                unsigned long long alignedPos = exactPos - (exactPos % 4096); 
                
                LARGE_INTEGER newDiskPos;
                newDiskPos.QuadPart = alignedPos;
                SetFilePointerEx(hDevice, newDiskPos, NULL, FILE_BEGIN);
                
                globalOffset = alignedPos;
                skipBytesInNextChunk = (unsigned int)(exactPos - alignedPos); 
                break; 
            } else {
                break;
            }
        }

        if (!fileFoundInChunk) {
            globalOffset += bytesRead;
        }

        if (totalDiskSize > 0) {
            float progress = ((float)globalOffset / totalDiskSize) * 100.0f;
            if (progress > 100.0f) progress = 100.0f;

            int barWidth = 40;
            int pos = (int)(barWidth * progress / 100.0f);
            
            printf("\r[");
            for (int i = 0; i < barWidth; ++i) {
                if (i < pos) printf("=");
                else if (i == pos) printf(">");
                else printf(" ");
            }
            printf("] %5.2f%% (%llu МБ)    ", progress, globalOffset / (1024 * 1024));
            fflush(stdout); 
        }

        if (_kbhit()) {
            char key = _getch();
            if (key == 'p' || key == 'P' || key == 'з' || key == 'З') { 
                printf("\n\n[||] ПАУЗА. Нажмите 'P' для продолжения...\n");
                while (1) {
                    if (_kbhit()) {
                        char resumeKey = _getch();
                        if (resumeKey == 'p' || resumeKey == 'P' || resumeKey == 'з' || resumeKey == 'З') {
                            printf("[>] Сканирование возобновлено!\n\n");
                            break;
                        }
                    }
                    Sleep(50); 
                }
            }
        }
    }

    printf("\n\n[*] Сканирование завершено. Восстановлено файлов: %d\n", recoveredCount);

    CloseHandle(hDevice);
    VirtualFree(buffer, 0, MEM_RELEASE);
    return 0;
}
