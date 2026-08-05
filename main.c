// ============================================================================
//  RECOVERY v2.0 — Универсальная утилита восстановления файлов с RAW-носителей
//  Поддержка: MP4, AVI, MKV, FLV, WMV, WAV, MP3, FLAC, OGG, MIDI, M4A,
//             HEIC, JPEG, PNG, GIF, WebP, TIFF, PSD, ZIP, PDF, DOC, RTF,
//             RAR, 7z, GZip, XZ, BZip2, SQLite
// ============================================================================

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <conio.h>

// ============================================================================
//  Структуры для запроса свойств накопителя (StorageDeviceProperty)
// ============================================================================

#ifndef IOCTL_STORAGE_QUERY_PROPERTY
#define IOCTL_STORAGE_QUERY_PROPERTY 0x002D1400
#endif

typedef struct {
    DWORD PropertyId;              // 0 = StorageDeviceProperty
    DWORD QueryType;               // 0 = PropertyStandardQuery
    BYTE  AdditionalParameters[4];
} MY_STORAGE_PROPERTY_QUERY;

typedef struct {
    DWORD Version;
    DWORD Size;
    BYTE  DeviceType;
    BYTE  DeviceTypeModifier;
    BYTE  RemovableMedia;
    BYTE  CommandQueueing;
    DWORD VendorIdOffset;
    DWORD ProductIdOffset;
    DWORD ProductRevisionOffset;
    DWORD SerialNumberOffset;
    DWORD BusType;
    DWORD RawPropertiesLength;
    BYTE  RawDeviceProperties[1];
} MY_STORAGE_DEVICE_DESCRIPTOR;

static const char* BusTypeName(DWORD bt) {
    switch (bt) {
        case 1:  return "SCSI";   case 2:  return "ATAPI";
        case 3:  return "ATA";    case 7:  return "USB";
        case 8:  return "RAID";   case 0xA: return "SAS";
        case 0xB: return "SATA";  case 0xC: return "SD";
        case 0xD: return "MMC";   case 0x11: return "NVMe";
        default: return "???";
    }
}

// ============================================================================
//  Таблица сигнатур файлов
// ============================================================================

typedef struct {
    unsigned int signature;        // Основная 4-байтовая сигнатура (little-endian uint32)
    unsigned int signatureMask;    // Маска (0 = точное совпадение 0xFFFFFFFF)
    unsigned int signature2;       // Вторичная проверка (0 = нет)
    int          sig2Offset;       // Смещение от checkPos для вторичной проверки
    const char*  extension;
    const char*  description;
    int          offsetCorrection; // Коррекция: сигнатура не в начале файла
    unsigned long long maxSize;
} FileSignature;

#define KB (1024ULL)
#define MB (1024ULL * 1024)
#define GB (1024ULL * 1024 * 1024)

FileSignature signatures[] = {
    // ─── ВИДЕО ──────────────────────────────────────────────────────────
    //    HEIC должен быть до generic MP4 (приоритет при совпадении сектора)
    { 0x70797466, 0, 0x63696568, 4, "heic","HEIC Image",      -4, 200*MB }, // ftyp + heic
    { 0x70797466, 0, 0x3166696D, 4, "heic","HEIC (mif1)",     -4, 200*MB }, // ftyp + mif1
    { 0x70797466, 0, 0x2041344D, 4, "m4a", "M4A Audio",       -4, 500*MB }, // ftyp + M4A
    { 0x70797466, 0, 0,          0, "mp4", "MPEG-4/MOV",      -4, 30*GB  }, // ftyp (catch-all)
    { 0x46464952, 0, 0x20495641, 8, "avi", "AVI Video",        0, 30*GB  }, // RIFF + AVI
    { 0xA3DF451A, 0, 0,          0, "mkv", "Matroska/WebM",    0, 30*GB  },
    { 0x01564C46, 0, 0,          0, "flv", "Flash Video",      0, 4*GB   }, // FLV\x01
    { 0x75B22630, 0, 0x11CF668E, 4, "wmv", "WMV/ASF",          0, 10*GB  }, // ASF GUID

    // ─── АУДИО ──────────────────────────────────────────────────────────
    { 0x46464952, 0, 0x45564157, 8, "wav", "WAV Audio",        0, 2*GB   }, // RIFF + WAVE
    { 0x03334449, 0, 0,          0, "mp3", "MP3 (ID3v2.3)",    0, 50*MB  }, // ID3\x03
    { 0x04334449, 0, 0,          0, "mp3", "MP3 (ID3v2.4)",    0, 50*MB  }, // ID3\x04
    { 0x43614C66, 0, 0,          0, "flac","FLAC Audio",       0, 500*MB }, // fLaC
    { 0x5367674F, 0, 0,          0, "ogg", "OGG Vorbis",       0, 500*MB }, // OggS
    { 0x6468544D, 0, 0,          0, "mid", "MIDI",             0, 10*MB  }, // MThd

    // ─── ИЗОБРАЖЕНИЯ ────────────────────────────────────────────────────
    { 0xE0FFD8FF, 0, 0,          0, "jpg", "JPEG (JFIF)",      0, 25*MB  },
    { 0xE1FFD8FF, 0, 0,          0, "jpg", "JPEG (EXIF)",      0, 25*MB  },
    { 0x474E5089, 0, 0,          0, "png", "PNG Image",        0, 50*MB  },
    { 0x38464947, 0, 0,          0, "gif", "GIF Image",        0, 30*MB  }, // GIF8
    { 0x46464952, 0, 0x50424557, 8, "webp","WebP Image",       0, 25*MB  }, // RIFF + WEBP
    { 0x002A4949, 0, 0,          0, "tif", "TIFF (LE)",        0, 500*MB }, // II*\0
    { 0x2A004D4D, 0, 0,          0, "tif", "TIFF (BE)",        0, 500*MB }, // MM\0*
    { 0x53504238, 0, 0,          0, "psd", "Photoshop PSD",    0, 2*GB   }, // 8BPS

    // ─── ДОКУМЕНТЫ ──────────────────────────────────────────────────────
    { 0x04034B50, 0, 0,          0, "zip", "ZIP/DOCX/XLSX",    0, 2*GB   },
    { 0x46445025, 0, 0,          0, "pdf", "PDF Document",     0, 500*MB },
    { 0xE011CFD0, 0, 0,          0, "doc", "MS Office (OLE)",  0, 200*MB }, // D0CF11E0
    { 0x74725C7B, 0, 0,          0, "rtf", "Rich Text",        0, 50*MB  }, // {\rt

    // ─── АРХИВЫ ─────────────────────────────────────────────────────────
    { 0x21726152, 0, 0,          0, "rar", "RAR Archive",      0, 4*GB   }, // Rar!
    { 0xAFBC7A37, 0, 0,          0, "7z",  "7-Zip Archive",    0, 4*GB   }, // 7z magic
    { 0x00088B1F, 0x00FFFFFF, 0, 0, "gz",  "GZip Archive",     0, 2*GB   }, // 1F 8B 08
    { 0x587A37FD, 0, 0,          0, "xz",  "XZ Archive",       0, 4*GB   }, // FD 37 7A 58
    { 0x00685A42, 0x00FFFFFF, 0, 0, "bz2", "BZip2 Archive",    0, 2*GB   }, // BZh

    // ─── БАЗЫ ДАННЫХ ────────────────────────────────────────────────────
    { 0x694C5153, 0, 0,          0, "sqlite","SQLite DB",       0, 2*GB   }, // SQLi
};

int sigCount = sizeof(signatures) / sizeof(FileSignature);

// Глобальные настройки
char g_outputDir[MAX_PATH] = "";

// ============================================================================
//  Безопасное чтение с диска (выровненное, динамический буфер)
// ============================================================================

BOOL ReadDiskSafe(HANDLE hDisk, unsigned long long offset, void* buffer, DWORD size) {
    DWORD SECTOR = 4096;
    unsigned long long alignedOffset = (offset / SECTOR) * SECTOR;
    DWORD diff = (DWORD)(offset - alignedOffset);
    DWORD readSize = ((diff + size + SECTOR - 1) / SECTOR) * SECTOR;

    if (readSize > 1 * MB) return FALSE;

    unsigned char* temp = (unsigned char*)VirtualAlloc(NULL, readSize, MEM_COMMIT, PAGE_READWRITE);
    if (!temp) return FALSE;

    LARGE_INTEGER pos;
    pos.QuadPart = alignedOffset;
    if (!SetFilePointerEx(hDisk, pos, NULL, FILE_BEGIN)) {
        VirtualFree(temp, 0, MEM_RELEASE);
        return FALSE;
    }

    DWORD br;
    BOOL result = FALSE;
    if (ReadFile(hDisk, temp, readSize, &br, NULL) && br >= diff + size) {
        memcpy(buffer, temp + diff, size);
        result = TRUE;
    }

    VirtualFree(temp, 0, MEM_RELEASE);
    return result;
}

// ============================================================================
//  Парсеры точного размера файла
// ============================================================================

// RIFF-контейнеры: AVI, WAV, WebP
unsigned long long ParseRiffSize(HANDLE hDisk, unsigned long long absoluteStart) {
    unsigned char header[8];
    if (ReadDiskSafe(hDisk, absoluteStart, header, 8)) {
        unsigned int reportedSize = *(unsigned int*)(header + 4);
        if (reportedSize > 0 && reportedSize < 30ULL * GB)
            return (unsigned long long)reportedSize + 8;
    }
    return 0;
}

// MP4/MOV: обход дерева атомов
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

        // Проверка: тип атома (байты 4-7) должен быть печатным ASCII
        int isAscii = 1;
        for (int i = 4; i < 8; i++) {
            if (header[i] < 0x20 || header[i] > 0x7E) isAscii = 0;
        }
        if (!isAscii) break;

        totalSize += boxSize;
        currentOffset += boxSize;

        if (totalSize > 50 * GB) return 0;
    }
    return totalSize;
}

// ============================================================================
//  Валидация сигнатур (вторичные проверки для снижения false positive)
// ============================================================================

int ValidateSignature(unsigned char* ptr, unsigned long long remainingBytes, const char* ext) {
    if (remainingBytes < 16) return 1;

    // MP4/MOV/HEIC/M4A: проверка что байты 4-7 (бренд) — ASCII
    if (strcmp(ext, "mp4") == 0 || strcmp(ext, "heic") == 0 || strcmp(ext, "m4a") == 0) {
        for (int i = 4; i < 8; i++) {
            unsigned char b = ptr[i];
            if (!((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
                  (b >= '0' && b <= '9') || b == ' '))
                return 0;
        }
        return 1;
    }
    // MKV/WebM: ищем "matr" или "webm" в первых 64 байтах
    if (strcmp(ext, "mkv") == 0) {
        for (int i = 4; i < 64 && (unsigned long long)(i + 4) < remainingBytes; i++) {
            if (ptr[i]=='m' && ptr[i+1]=='a' && ptr[i+2]=='t' && ptr[i+3]=='r') return 1;
            if (ptr[i]=='w' && ptr[i+1]=='e' && ptr[i+2]=='b' && ptr[i+3]=='m') return 1;
        }
        return 0;
    }
    // JPEG: проверка JFIF или Exif маркера
    if (strcmp(ext, "jpg") == 0) {
        if (ptr[6]=='J' && ptr[7]=='F' && ptr[8]=='I' && ptr[9]=='F') return 1;
        if (ptr[6]=='E' && ptr[7]=='x' && ptr[8]=='i' && ptr[9]=='f') return 1;
        return 0;
    }
    // GIF: проверка версии "87a" или "89a"
    if (strcmp(ext, "gif") == 0) {
        return ((ptr[4]=='7' || ptr[4]=='9') && ptr[5]=='a') ? 1 : 0;
    }
    // FLV: байт 4 — флаги (audio/video), байты 5-8 — размер заголовка
    if (strcmp(ext, "flv") == 0) {
        if ((ptr[4] & 0xFA) != 0) return 0; // Только биты 0 (video) и 2 (audio)
        unsigned int headerSize = (ptr[5]<<24)|(ptr[6]<<16)|(ptr[7]<<8)|ptr[8];
        return (headerSize == 9) ? 1 : 0;
    }
    // MP3 (ID3): проверка флагов (байт 5) и synchsafe size
    if (strcmp(ext, "mp3") == 0) {
        if (ptr[5] > 0x1F) return 0; // Неизвестные флаги
        // Байты 6-9: synchsafe integer (каждый байт < 0x80)
        for (int i = 6; i < 10 && (unsigned long long)i < remainingBytes; i++) {
            if (ptr[i] & 0x80) return 0;
        }
        return 1;
    }
    // OGG: байт 4 = версия (всегда 0)
    if (strcmp(ext, "ogg") == 0) {
        return (ptr[4] == 0x00) ? 1 : 0;
    }
    // MIDI: длина заголовка = 6 (байты 4-7: 00 00 00 06)
    if (strcmp(ext, "mid") == 0) {
        return (ptr[4]==0 && ptr[5]==0 && ptr[6]==0 && ptr[7]==6) ? 1 : 0;
    }
    // PSD: версия = 1 (байты 4-5: 00 01)
    if (strcmp(ext, "psd") == 0) {
        return (ptr[4]==0 && ptr[5]==1) ? 1 : 0;
    }
    // OLE2 (DOC/XLS/PPT): вторые 4 байта магического числа
    if (strcmp(ext, "doc") == 0) {
        return (ptr[4]==0xA1 && ptr[5]==0xB1 && ptr[6]==0x1A && ptr[7]==0xE1) ? 1 : 0;
    }
    // RTF: проверка "{\rtf" (байт 4 = 'f')
    if (strcmp(ext, "rtf") == 0) {
        return (ptr[4] == 'f') ? 1 : 0;
    }
    // RAR: байты 4-5 = 1A 07
    if (strcmp(ext, "rar") == 0) {
        return (ptr[4]==0x1A && ptr[5]==0x07) ? 1 : 0;
    }
    // 7z: байты 4-5 = 27 1C
    if (strcmp(ext, "7z") == 0) {
        return (ptr[4]==0x27 && ptr[5]==0x1C) ? 1 : 0;
    }
    // GZip: флаги (байт 3) ≤ 0x1F
    if (strcmp(ext, "gz") == 0) {
        return (ptr[3] <= 0x1F) ? 1 : 0;
    }
    // BZip2: уровень сжатия '1'-'9' (байт 3)
    if (strcmp(ext, "bz2") == 0) {
        return (ptr[3] >= '1' && ptr[3] <= '9') ? 1 : 0;
    }
    // XZ: байты 4-5 = 5A 00
    if (strcmp(ext, "xz") == 0) {
        return (ptr[4]==0x5A && ptr[5]==0x00) ? 1 : 0;
    }
    // SQLite: проверка "te f" (байты 4-7)
    if (strcmp(ext, "sqlite") == 0) {
        return (ptr[4]=='t' && ptr[5]=='e' && ptr[6]==' ' && ptr[7]=='f') ? 1 : 0;
    }
    // TIFF: проверка IFD offset (байты 4-7) — разумный диапазон
    if (strcmp(ext, "tif") == 0) {
        unsigned int ifdOffset;
        if (ptr[0] == 'I' && ptr[1] == 'I') { // Little-endian
            ifdOffset = *(unsigned int*)(ptr + 4);
        } else { // Big-endian
            ifdOffset = (ptr[4]<<24)|(ptr[5]<<16)|(ptr[6]<<8)|ptr[7];
        }
        return (ifdOffset >= 8 && ifdOffset < 65536) ? 1 : 0;
    }
    return 1;
}

// ============================================================================
//  Поиск сигнатуры в буфере (секторный шаг, с поддержкой mask и sig2)
// ============================================================================

long long FindValidSignatureFast(unsigned char* buffer, unsigned long long size, int sigIndex) {
    int correction = signatures[sigIndex].offsetCorrection;
    unsigned int mask = signatures[sigIndex].signatureMask ? signatures[sigIndex].signatureMask : 0xFFFFFFFF;

    for (unsigned long long sector = 0; sector + 512 <= size; sector += 512) {
        long long checkPos = (long long)sector - correction;

        if (checkPos >= 0 && checkPos + 64 <= (long long)size) {
            unsigned int val = *(unsigned int*)(buffer + checkPos);

            if ((val & mask) == (signatures[sigIndex].signature & mask)) {
                // Вторичная проверка (RIFF sub-type, ASF GUID и т.д.)
                if (signatures[sigIndex].signature2 != 0) {
                    long long sig2Pos = checkPos + signatures[sigIndex].sig2Offset;
                    if (sig2Pos < 0 || sig2Pos + 4 > (long long)size) continue;
                    unsigned int val2 = *(unsigned int*)(buffer + sig2Pos);
                    if (val2 != signatures[sigIndex].signature2) continue;
                }
                if (ValidateSignature(buffer + checkPos, size - checkPos, signatures[sigIndex].extension)) {
                    return (long long)sector;
                }
            }
        }
    }
    return -1;
}

// ============================================================================
//  Поиск трейлеров (конец файла)
// ============================================================================

long long FindTrailer(unsigned char* buffer, DWORD size, const char* ext) {
    // PDF: %%EOF
    if (strcmp(ext, "pdf") == 0) {
        long long lastPos = -1;
        for (DWORD i = 0; i + 5 <= size; i++) {
            if (buffer[i]=='%' && buffer[i+1]=='%' && buffer[i+2]=='E' &&
                buffer[i+3]=='O' && buffer[i+4]=='F') {
                lastPos = i + 5;
            }
        }
        return lastPos;
    }
    // ZIP: End of Central Directory (PK\x05\x06) + comment length
    if (strcmp(ext, "zip") == 0) {
        long long lastPos = -1;
        for (DWORD i = 0; i + 22 <= size; i++) {
            if (buffer[i]==0x50 && buffer[i+1]==0x4B &&
                buffer[i+2]==0x05 && buffer[i+3]==0x06) {
                unsigned short commentLen = *(unsigned short*)(buffer + i + 20);
                DWORD endPos = i + 22 + commentLen;
                if (endPos <= size) lastPos = endPos;
            }
        }
        return lastPos;
    }
    // PNG: IEND chunk (00 00 00 00 49 45 4E 44 AE 42 60 82)
    if (strcmp(ext, "png") == 0) {
        for (DWORD i = 0; i + 12 <= size; i++) {
            if (buffer[i]==0x00 && buffer[i+1]==0x00 && buffer[i+2]==0x00 && buffer[i+3]==0x00 &&
                buffer[i+4]==0x49 && buffer[i+5]==0x45 && buffer[i+6]==0x4E && buffer[i+7]==0x44 &&
                buffer[i+8]==0xAE && buffer[i+9]==0x42 && buffer[i+10]==0x60 && buffer[i+11]==0x82) {
                return i + 12;
            }
        }
    }
    // JPEG: EOI (FF D9) — последнее вхождение
    if (strcmp(ext, "jpg") == 0) {
        long long lastPos = -1;
        for (DWORD i = 0; i + 1 < size; i++) {
            if (buffer[i]==0xFF && buffer[i+1]==0xD9) {
                lastPos = i + 2;
            }
        }
        return lastPos;
    }
    // GIF: трейлер 0x3B (после блоков данных)
    if (strcmp(ext, "gif") == 0) {
        long long lastPos = -1;
        for (DWORD i = 0; i < size; i++) {
            if (buffer[i] == 0x3B) lastPos = i + 1;
        }
        return lastPos;
    }
    return -1;
}

// ============================================================================
//  Извлечение даты из EXIF (для генерации имён JPEG)
// ============================================================================

int ExtractExifDate(unsigned char* data, DWORD size, char* outDate) {
    if (size < 20 || data[0] != 0xFF || data[1] != 0xD8) return 0;

    DWORD pos = 2;
    while (pos + 4 < size && pos < 65536) {
        if (data[pos] != 0xFF) break;
        unsigned char marker = data[pos + 1];
        unsigned short segLen = (data[pos + 2] << 8) | data[pos + 3];

        if (marker == 0xE1 && segLen > 8) { // APP1 = EXIF
            DWORD segStart = pos + 4;
            DWORD segEnd = segStart + segLen - 2;
            if (segEnd > size) segEnd = size;

            // Поиск паттерна "YYYY:MM:DD HH:MM:SS" внутри EXIF-сегмента
            for (DWORD i = segStart; i + 19 <= segEnd; i++) {
                if (data[i] >= '1' && data[i] <= '2' &&
                    data[i+4] == ':' && data[i+7] == ':' &&
                    data[i+10] == ' ' && data[i+13] == ':' && data[i+16] == ':') {
                    int digitPos[] = {0,1,2,3, 5,6, 8,9, 11,12, 14,15, 17,18};
                    int valid = 1;
                    for (int j = 0; j < 14; j++) {
                        unsigned char c = data[i + digitPos[j]];
                        if (c < '0' || c > '9') { valid = 0; break; }
                    }
                    if (valid) {
                        memcpy(outDate, data + i, 19);
                        outDate[19] = '\0';
                        return 1;
                    }
                }
            }
        }
        if (marker == 0xDA) break; // SOS — дальше сжатые данные
        pos += 2 + segLen;
    }
    return 0;
}

// ============================================================================
//  Вспомогательные функции
// ============================================================================

// Префикс для имени файла по типу
static const char* GetPrefix(const char* ext) {
    if (strcmp(ext,"jpg")==0 || strcmp(ext,"png")==0 || strcmp(ext,"gif")==0 ||
        strcmp(ext,"webp")==0 || strcmp(ext,"tif")==0 || strcmp(ext,"psd")==0 ||
        strcmp(ext,"heic")==0)
        return "IMG";
    if (strcmp(ext,"mp4")==0 || strcmp(ext,"avi")==0 || strcmp(ext,"mkv")==0 ||
        strcmp(ext,"flv")==0 || strcmp(ext,"wmv")==0)
        return "VID";
    if (strcmp(ext,"wav")==0 || strcmp(ext,"mp3")==0 || strcmp(ext,"flac")==0 ||
        strcmp(ext,"ogg")==0 || strcmp(ext,"mid")==0 || strcmp(ext,"m4a")==0)
        return "AUD";
    if (strcmp(ext,"pdf")==0 || strcmp(ext,"doc")==0 || strcmp(ext,"rtf")==0)
        return "DOC";
    if (strcmp(ext,"zip")==0 || strcmp(ext,"rar")==0 || strcmp(ext,"7z")==0 ||
        strcmp(ext,"gz")==0 || strcmp(ext,"xz")==0 || strcmp(ext,"bz2")==0)
        return "ARC";
    if (strcmp(ext,"sqlite")==0)
        return "DB";
    return "FILE";
}

// Рекурсивное создание директорий
static void CreateDirRecursive(const char* path) {
    char tmp[MAX_PATH];
    strncpy(tmp, path, MAX_PATH - 1);
    tmp[MAX_PATH - 1] = '\0';
    for (int i = 0; tmp[i]; i++) {
        if ((tmp[i] == '\\' || tmp[i] == '/') && i > 0) {
            char saved = tmp[i];
            tmp[i] = '\0';
            CreateDirectoryA(tmp, NULL);
            tmp[i] = saved;
        }
    }
    CreateDirectoryA(tmp, NULL);
}

// Обрезка пробелов справа
static void TrimRight(char* str) {
    int len = (int)strlen(str);
    while (len > 0 && (str[len-1] == ' ' || str[len-1] == '\t' || str[len-1] == '\n' || str[len-1] == '\r'))
        str[--len] = '\0';
}

// ============================================================================
//  Извлечение файла с диска
// ============================================================================

unsigned long long ExtractFile(HANDLE hDisk, unsigned long long absoluteOffset,
                                const char* ext, const char* description,
                                int fileId, unsigned long long maxSize) {
    // --- Предварительное чтение EXIF-даты (для JPEG) ---
    char datePart[32] = "";
    if (strcmp(ext, "jpg") == 0) {
        DWORD EXIF_BUF = 65536;
        unsigned char* exifBuf = (unsigned char*)VirtualAlloc(NULL, EXIF_BUF, MEM_COMMIT, PAGE_READWRITE);
        if (exifBuf) {
            LARGE_INTEGER ep; ep.QuadPart = absoluteOffset;
            SetFilePointerEx(hDisk, ep, NULL, FILE_BEGIN);
            DWORD er;
            if (ReadFile(hDisk, exifBuf, EXIF_BUF, &er, NULL) && er > 0) {
                char rawDate[20] = {0};
                if (ExtractExifDate(exifBuf, er, rawDate)) {
                    sprintf(datePart, "_%c%c%c%c%c%c%c%c_%c%c%c%c%c%c",
                        rawDate[0],rawDate[1],rawDate[2],rawDate[3],   // YYYY
                        rawDate[5],rawDate[6],rawDate[8],rawDate[9],   // MMDD
                        rawDate[11],rawDate[12],rawDate[14],rawDate[15],// HHMM
                        rawDate[17],rawDate[18]);                      // SS
                }
            }
            VirtualFree(exifBuf, 0, MEM_RELEASE);
        }
    }

    // --- Генерация имени файла ---
    char filename[512];
    const char* prefix = GetPrefix(ext);
    if (g_outputDir[0]) {
        sprintf(filename, "%s\\%s%s_%03d.%s", g_outputDir, prefix, datePart, fileId, ext);
    } else {
        sprintf(filename, "%s%s_%03d.%s", prefix, datePart, fileId, ext);
    }

    // --- Определение точного размера (если возможно) ---
    unsigned long long exactSize = 0;
    int useBlade = 1;

    if (strcmp(ext, "mp4") == 0 || strcmp(ext, "heic") == 0 || strcmp(ext, "m4a") == 0)
        exactSize = ParseMp4Size(hDisk, absoluteOffset);
    else if (strcmp(ext, "avi") == 0 || strcmp(ext, "wav") == 0 || strcmp(ext, "webp") == 0)
        exactSize = ParseRiffSize(hDisk, absoluteOffset);

    unsigned long long bytesToRead = maxSize;

    if (exactSize > 0) {
        printf("\n[>>>] %s -> %s (%.2f MB, %s)...\n", description, filename, exactSize / (1024.0 * 1024.0), "точный размер");
        printf("    [i] Бронированный режим (защита от фрагментации).\n");
        bytesToRead = exactSize;
        useBlade = 0;
    } else {
        printf("\n[>>>] %s -> %s (ожидание обрезки)...\n", description, filename);
    }
    fflush(stdout);

    // --- Открытие выходного файла ---
    HANDLE hOutFile = CreateFileA(filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOutFile == INVALID_HANDLE_VALUE) {
        printf("    [-] Ошибка создания файла %s!\n", filename);
        return 0;
    }

    // --- Перемотка на начало файла ---
    LARGE_INTEGER targetPos;
    targetPos.QuadPart = absoluteOffset;
    SetFilePointerEx(hDisk, targetPos, NULL, FILE_BEGIN);

    // --- Основной цикл чтения/записи ---
    unsigned long long totalWritten = 0;
    DWORD bufferSize = 32 * 1024 * 1024;
    unsigned char* copyBuffer = (unsigned char*)VirtualAlloc(NULL, bufferSize, MEM_COMMIT, PAGE_READWRITE);
    if (!copyBuffer) {
        printf("    [-] Ошибка выделения памяти!\n");
        CloseHandle(hOutFile);
        return 0;
    }
    int isFirstBlock = 1;

    while (bytesToRead > 0) {
        DWORD attemptRead = bufferSize;
        DWORD bytesRead = 0, bytesWritten = 0;

        if (ReadFile(hDisk, copyBuffer, attemptRead, &bytesRead, NULL) && bytesRead > 0) {

            DWORD validDataLen = (bytesRead > bytesToRead) ? (DWORD)bytesToRead : bytesRead;

            DWORD startCheckOffset = isFirstBlock ? 512 : 0;
            long long bestAlienSector = -1;
            int bestAlienIndex = -1;

            // Поиск трейлера (для форматов с известным окончанием)
            if (strcmp(ext, "zip") == 0 || strcmp(ext, "pdf") == 0 ||
                strcmp(ext, "png") == 0 || strcmp(ext, "jpg") == 0 ||
                strcmp(ext, "gif") == 0) {
                long long trailerPos = FindTrailer(copyBuffer + startCheckOffset,
                                                    validDataLen - startCheckOffset, ext);
                if (trailerPos != -1) {
                    trailerPos += startCheckOffset;
                    long long safeCut = trailerPos + 512;
                    if (safeCut > (long long)bytesRead) safeCut = bytesRead;

                    printf("\r    [!] Найден маркер конца файла (%s).            \n", ext);
                    validDataLen = (DWORD)safeCut;
                    bytesToRead = validDataLen;
                    useBlade = 0;
                }
            }

            // Поиск «чужих» сигнатур (blade-обрезка)
            if (useBlade && validDataLen > startCheckOffset) {
                for (int i = 0; i < sigCount; i++) {
                    // Не обрезаем контейнеры по встроенным форматам
                    if (strcmp(ext, "zip") == 0 || strcmp(ext, "pdf") == 0 ||
                        strcmp(ext, "doc") == 0 || strcmp(ext, "rtf") == 0) {
                        if (strcmp(signatures[i].extension, "jpg") == 0 ||
                            strcmp(signatures[i].extension, "png") == 0 ||
                            strcmp(signatures[i].extension, "gif") == 0 ||
                            strcmp(signatures[i].extension, "pdf") == 0 ||
                            strcmp(signatures[i].extension, "zip") == 0)
                            continue;
                    }

                    long long alienSector = FindValidSignatureFast(
                        copyBuffer + startCheckOffset,
                        validDataLen - startCheckOffset, i);

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

                printf("\r    [!] Обрезка на байте %llu (начало %s)      \n",
                       alienAbsolute, signatures[bestAlienIndex].extension);

                validDataLen = (DWORD)cutPos;
                bytesToRead = validDataLen;
            }

            isFirstBlock = 0;

            // Защита от underflow
            if ((unsigned long long)validDataLen > bytesToRead)
                validDataLen = (DWORD)bytesToRead;

            WriteFile(hOutFile, copyBuffer, validDataLen, &bytesWritten, NULL);
            totalWritten += bytesWritten;
            bytesToRead -= validDataLen;

            if (totalWritten % (32 * MB) == 0 || bytesToRead == 0) {
                printf("\r    [~] Сохранено: %llu MB...", totalWritten / MB);
                fflush(stdout);
            }
        } else {
            break;
        }
    }

    VirtualFree(copyBuffer, 0, MEM_RELEASE);
    CloseHandle(hOutFile);

    printf("\r[+] %s — %.2f MB                                      \n\n",
           filename, totalWritten / (1024.0 * 1024.0));
    fflush(stdout);
    return totalWritten;
}

// ============================================================================
//  Список физических дисков
// ============================================================================

typedef struct {
    int driveNumber;
    unsigned long long sizeBytes;
    char label[128];
    char busType[16];
    int removable;
} DiskInfo;

int ListPhysicalDisks(DiskInfo* disks, int maxDisks) {
    int count = 0;
    for (int i = 0; i < 32 && count < maxDisks; i++) {
        char path[64];
        sprintf(path, "\\\\.\\PhysicalDrive%d", i);

        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        DiskInfo* d = &disks[count];
        d->driveNumber = i;
        d->sizeBytes = 0;
        d->label[0] = '\0';
        strcpy(d->busType, "???");
        d->removable = 0;

        // Размер диска
        GET_LENGTH_INFORMATION gli;
        DWORD br;
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &gli, sizeof(gli), &br, NULL))
            d->sizeBytes = gli.Length.QuadPart;

        // Модель и тип шины
        char descBuf[4096] = {0};
        MY_STORAGE_PROPERTY_QUERY query = {0};
        query.PropertyId = 0; // StorageDeviceProperty
        query.QueryType  = 0; // PropertyStandardQuery

        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                            descBuf, sizeof(descBuf), &br, NULL)) {
            MY_STORAGE_DEVICE_DESCRIPTOR* desc = (MY_STORAGE_DEVICE_DESCRIPTOR*)descBuf;

            char vendor[64] = "", product[64] = "";
            if (desc->VendorIdOffset && desc->VendorIdOffset < sizeof(descBuf))
                strncpy(vendor, descBuf + desc->VendorIdOffset, sizeof(vendor) - 1);
            if (desc->ProductIdOffset && desc->ProductIdOffset < sizeof(descBuf))
                strncpy(product, descBuf + desc->ProductIdOffset, sizeof(product) - 1);

            TrimRight(vendor);
            TrimRight(product);

            if (vendor[0] && product[0])
                sprintf(d->label, "%s %s", vendor, product);
            else if (product[0])
                strcpy(d->label, product);
            else if (vendor[0])
                strcpy(d->label, vendor);
            else
                sprintf(d->label, "PhysicalDrive%d", i);

            strcpy(d->busType, BusTypeName(desc->BusType));
            d->removable = desc->RemovableMedia;
        } else {
            sprintf(d->label, "PhysicalDrive%d", i);
        }

        CloseHandle(h);
        count++;
    }
    return count;
}

// ============================================================================
//  Точка входа
// ============================================================================

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // --- Баннер ---
    printf("\n");
    printf("  ╔════════════════════════════════════════════════════════════╗\n");
    printf("  ║       RECOVERY v2.0 — Восстановление файлов с RAW        ║\n");
    printf("  ╚════════════════════════════════════════════════════════════╝\n\n");

    printf("  Форматы: mp4 avi mkv flv wmv | wav mp3 flac ogg mid m4a\n");
    printf("           jpg png gif webp tif psd heic | zip pdf doc rtf\n");
    printf("           rar 7z gz xz bz2 | sqlite\n\n");

    // --- Список дисков ---
    DiskInfo disks[32];
    int diskCount = ListPhysicalDisks(disks, 32);

    if (diskCount == 0) {
        printf("[-] Диски не обнаружены. Запустите от имени администратора.\n");
        return 1;
    }

    printf("[*] Обнаруженные диски:\n");
    for (int i = 0; i < diskCount; i++) {
        printf("    [%d] %s (%.2f GB) [%s]%s\n",
               disks[i].driveNumber,
               disks[i].label,
               disks[i].sizeBytes / (1024.0 * 1024.0 * 1024.0),
               disks[i].busType,
               disks[i].removable ? " [Removable]" : "");
    }

    // --- Выбор диска ---
    int selectedDrive = -1;
    printf("\nВведите номер диска для сканирования: ");
    fflush(stdout);
    if (scanf("%d", &selectedDrive) != 1) {
        printf("[-] Неверный ввод.\n");
        return 1;
    }
    // Очистка буфера ввода
    while (getchar() != '\n');

    // Проверка что диск существует
    int driveFound = 0;
    unsigned long long totalDiskSize = 0;
    for (int i = 0; i < diskCount; i++) {
        if (disks[i].driveNumber == selectedDrive) {
            driveFound = 1;
            totalDiskSize = disks[i].sizeBytes;
            break;
        }
    }
    if (!driveFound) {
        printf("[-] Диск %d не найден.\n", selectedDrive);
        return 1;
    }

    // --- Выбор директории сохранения ---
    printf("Папка для сохранения (Enter = текущая): ");
    fflush(stdout);
    char dirInput[MAX_PATH] = "";
    if (fgets(dirInput, sizeof(dirInput), stdin)) {
        TrimRight(dirInput);
        if (dirInput[0]) {
            // Убрать завершающий слеш
            int len = (int)strlen(dirInput);
            if (len > 0 && (dirInput[len-1] == '\\' || dirInput[len-1] == '/'))
                dirInput[len-1] = '\0';
            strncpy(g_outputDir, dirInput, MAX_PATH - 1);
            CreateDirRecursive(g_outputDir);
            printf("[*] Файлы будут сохранены в: %s\n", g_outputDir);
        }
    }

    // --- Открытие диска ---
    char drivePath[64];
    sprintf(drivePath, "\\\\.\\PhysicalDrive%d", selectedDrive);

    HANDLE hDevice = CreateFileA(drivePath, GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        printf("[-] Не удалось открыть диск %d. Запустите от имени администратора.\n", selectedDrive);
        return 1;
    }

    printf("\n[*] Диск открыт: PhysicalDrive%d (%.2f GB)\n",
           selectedDrive, totalDiskSize / (1024.0 * 1024.0 * 1024.0));
    printf("[*] Сканирование... (P = пауза)\n\n");

    // --- Основной цикл сканирования ---
    const unsigned long long CHUNK_SIZE = 32 * MB;
    unsigned char* buffer = (unsigned char*)VirtualAlloc(NULL, CHUNK_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!buffer) {
        printf("[-] Ошибка выделения памяти для буфера.\n");
        CloseHandle(hDevice);
        return 1;
    }

    unsigned long long globalOffset = 0;
    DWORD bytesRead = 0;
    int recoveredCount = 0;
    unsigned int skipBytesInNextChunk = 0;

    while (1) {
        if (!ReadFile(hDevice, buffer, (DWORD)CHUNK_SIZE, &bytesRead, NULL)) break;
        if (bytesRead == 0) break;

        long long localSearchOffset = skipBytesInNextChunk;
        skipBytesInNextChunk = 0;
        int fileFoundInChunk = 0;

        while (localSearchOffset < (long long)bytesRead) {
            long long bestFoundSector = -1;
            int bestSigIndex = -1;

            for (int i = 0; i < sigCount; i++) {
                long long foundSector = FindValidSignatureFast(
                    buffer + localSearchOffset,
                    bytesRead - (DWORD)localSearchOffset, i);

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

                printf("\r                                                                          \r");
                printf("[!] #%d — %s на отметке %llu байт\n",
                       recoveredCount, signatures[bestSigIndex].description, actualFileStart);
                fflush(stdout);

                unsigned long long extractedSize = ExtractFile(
                    hDevice, actualFileStart,
                    signatures[bestSigIndex].extension,
                    signatures[bestSigIndex].description,
                    recoveredCount,
                    signatures[bestSigIndex].maxSize);

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

        // --- Прогресс-бар ---
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
            printf("] %5.2f%% (%llu MB) | Найдено: %d   ",
                   progress, globalOffset / MB, recoveredCount);
            fflush(stdout);
        }

        // --- Пауза по нажатию P ---
        if (_kbhit()) {
            char key = _getch();
            if (key == 'p' || key == 'P' || key == (char)0xE7 || key == (char)0xC7) {
                printf("\n\n[||] ПАУЗА. Нажмите 'P' для продолжения...\n");
                while (1) {
                    if (_kbhit()) {
                        char rk = _getch();
                        if (rk=='p' || rk=='P' || rk==(char)0xE7 || rk==(char)0xC7) {
                            printf("[>] Сканирование возобновлено!\n\n");
                            break;
                        }
                    }
                    Sleep(50);
                }
            }
        }
    }

    printf("\n\n╔══════════════════════════════════════════╗\n");
    printf("║  Сканирование завершено!                 ║\n");
    printf("║  Восстановлено файлов: %-5d             ║\n", recoveredCount);
    if (g_outputDir[0])
        printf("║  Сохранено в: %-26s ║\n", g_outputDir);
    printf("╚══════════════════════════════════════════╝\n");

    CloseHandle(hDevice);
    VirtualFree(buffer, 0, MEM_RELEASE);
    return 0;
}
