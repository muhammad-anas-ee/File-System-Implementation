#include <iostream>
#include <fstream>
#include <cstring>
#include <string>
#include <iomanip>
#include <cmath>
#include <cstdlib>
using namespace std;
/*
 Uses linked lists and dynamic arrays only.
 On-disk layout:
 - 10 MB total: 1MB directory region, 1MB free-list region, 8MB data region
 - Directory entry = 500 bytes:
    bytes 0..255 : filename (null-terminated)
    bytes 256..259 : startBlock (int)
    bytes 260..263 : fileSize (int)
 - Block size = 1024 bytes
    first 4 bytes = nextBlock int (pointer)
    remaining 1020 bytes = data
*/
const string FS_FILENAME = "File_system.dat";

const int TOTAL_SIZE = 10 * 1024 * 1024;
const int DIR_REGION_SIZE = 1 * 1024 * 1024;
const int FREE_REGION_SIZE = 1 * 1024 * 1024;
const int DATA_REGION_SIZE = 8 * 1024 * 1024;
const int BLOCK_SIZE = 1024;
const int TOTAL_BLOCKS = DATA_REGION_SIZE / BLOCK_SIZE; // 8192

const int DIR_ENTRY_SIZE = 500;
const int MAX_FILENAME_BYTES = 256; // 255 chars + null
const int BLOCK_PTR_SIZE = sizeof(int);
const int BLOCK_DATA_SIZE = BLOCK_SIZE - BLOCK_PTR_SIZE;

int my_min(int a, int b) { 
    return (a < b) ? a : b;
}
/* ---------------- Linked list for directory (in-memory) ---------------- */
struct DirNode {
    char filename[MAX_FILENAME_BYTES];
    int startBlock;
    int fileSize;
    DirNode* next;
    DirNode* prev;
    DirNode() {
        filename[0] = '\0';
        startBlock = -1;
        fileSize = 0;
        next = prev = NULL;
    }
};
DirNode* dirHead = NULL;
DirNode* dirTail = NULL;
/* ---------------- Linked list for free blocks (in-memory) ---------------- */
struct FreeNode {
    int blockNum;
    FreeNode* next;
    FreeNode(int b) { blockNum = b; next = NULL; }
};
FreeNode* freeHead = NULL;
/* ---------------- Temporary list for defragmentation ---------------- */
struct FileTemp {
    char filename[MAX_FILENAME_BYTES];
    char* data; // dynamically allocated buffer
    int dataSize;
    int newStart; // new starting block after defrag
    FileTemp* next;
    FileTemp() { 
        filename[0] = '\0';
        data = NULL;
        dataSize = 0; 
        newStart = -1;
        next = NULL; 
    }
};
/* ---------------- Global file stream ---------------- */
fstream fs;
/* ---------------- Utility functions ---------------- */
int getBlockAddress(int blockNum) {
    return DIR_REGION_SIZE + FREE_REGION_SIZE + (blockNum * BLOCK_SIZE);
}

/* ---------------- Directory in-memory helpers ---------------- */

void addDirNodeMem(const char* fname, int startBlock, int fileSize) {
    DirNode* n = new DirNode();
    strncpy_s(n->filename, fname, MAX_FILENAME_BYTES - 1);
    n->filename[MAX_FILENAME_BYTES - 1] = '\0';
    n->startBlock = startBlock;
    n->fileSize = fileSize;
    n->next = n->prev = NULL;
    if (!dirHead) {
        dirHead = dirTail = n;
    }
    else {
        dirTail->next = n;
        n->prev = dirTail;
        dirTail = n;
    }
}

DirNode* findDirNodeMem(const char* fname) {
    DirNode* cur = dirHead;
    while (cur) {
        if (strcmp(cur->filename, fname) == 0) return cur;
        cur = cur->next;
    }
    return NULL;
}

bool deleteDirNodeMem(const char* fname) {
    DirNode* node = findDirNodeMem(fname);
    if (!node) return false;
    if (node->prev) node->prev->next = node->next; else dirHead = node->next;
    if (node->next) node->next->prev = node->prev; else dirTail = node->prev;
    delete node;
    return true;
}

/* ---------------- Free list helpers (linked list) ---------------- */

void initFreeListAll() {
    // freeHead will contain blocks in stack order: back is top when using pop from head
    // We'll add from highest to lowest so allocation pops low indexes (but order doesn't matter)
    freeHead = NULL;
    for (int i = TOTAL_BLOCKS - 1; i >= 0; --i) {
        FreeNode* n = new FreeNode(i);
        n->next = freeHead;
        freeHead = n;
    }
}

int countFreeBlocks() {
    int cnt = 0;
    FreeNode* cur = freeHead;
    while (cur) { cnt++; cur = cur->next; }
    return cnt;
}

int allocateBlock() {
    if (!freeHead) return -1;
    int b = freeHead->blockNum;
    FreeNode* tmp = freeHead;
    freeHead = freeHead->next;
    delete tmp;
    return b;
}

void deallocateBlock(int blockNum) {
    if (blockNum < 0 || blockNum >= TOTAL_BLOCKS) return;
    FreeNode* n = new FreeNode(blockNum);
    n->next = freeHead;
    freeHead = n;
}

/* ---------------- Persistence: directory and free list ---------------- */

void saveDirectoryToFS() {
    fs.seekp(0, ios::beg);
    char entry[DIR_ENTRY_SIZE];
    memset(entry, 0, DIR_ENTRY_SIZE);
    DirNode* cur = dirHead;
    int writtenEntries = 0;
    int maxEntries = DIR_REGION_SIZE / DIR_ENTRY_SIZE;
    while (cur && writtenEntries < maxEntries) {
        memset(entry, 0, DIR_ENTRY_SIZE);
        // filename
        strncpy_s(entry, cur->filename, MAX_FILENAME_BYTES - 1);
        int offsetStart = 256;
        memcpy(entry + offsetStart, &cur->startBlock, sizeof(int));
        memcpy(entry + offsetStart + 4, &cur->fileSize, sizeof(int));
        fs.write(entry, DIR_ENTRY_SIZE);
        cur = cur->next;
        writtenEntries++;
    }
    // fill rest with zeros
    int remain = DIR_REGION_SIZE - (writtenEntries * DIR_ENTRY_SIZE);
    if (remain > 0) {
        char* zeros = new char[remain];
        memset(zeros, 0, remain);
        fs.write(zeros, remain);
        delete[] zeros;
    }
    fs.flush();
}

void loadDirectoryFromFS() {
    // clear in-memory directory first
    DirNode* cur = dirHead;
    while (cur) {
        DirNode* nx = cur->next;
        delete cur;
        cur = nx;
    }
    dirHead = dirTail = NULL;
    fs.seekg(0, ios::beg);
    int entries = DIR_REGION_SIZE / DIR_ENTRY_SIZE;
    char entry[DIR_ENTRY_SIZE];
    for (int i = 0; i < entries; ++i) {
        fs.read(entry, DIR_ENTRY_SIZE);
        if (!fs) break;
        // if first byte zero -> empty; skip but still continue (some later entries might exist)
        if (entry[0] == 0) continue;
        char fname[MAX_FILENAME_BYTES];
        memset(fname, 0, MAX_FILENAME_BYTES);
        strncpy_s(fname, entry, MAX_FILENAME_BYTES - 1);
        int startBlock = -1;
        int fileSize = 0;
        int offsetStart = 256;
        memcpy(&startBlock, entry + offsetStart, sizeof(int));
        memcpy(&fileSize, entry + offsetStart + 4, sizeof(int));
        if (fname[0] != '\0') addDirNodeMem(fname, startBlock, fileSize);
    }
}

/* Save free list as ints in FREE_REGION_SIZE; use -1 termination; rest filled with -1 */
void saveFreeListToFS() {
    fs.seekp(DIR_REGION_SIZE, ios::beg);
    // write free blocks
    FreeNode* cur = freeHead;
    while (cur) {
        int b = cur->blockNum;
        fs.write(reinterpret_cast<char*>(&b), sizeof(int));
        cur = cur->next;
    }
    // write terminator -1
    int minusOne = -1;
    fs.write(reinterpret_cast<char*>(&minusOne), sizeof(int));
    // fill remaining region with -1
    int written = (countFreeBlocks() + 1) * sizeof(int);
    int remaining = FREE_REGION_SIZE - written;
    if (remaining > 0) {
        int countInts = remaining / sizeof(int);
        int* fill = new int[countInts];
        for (int i = 0; i < countInts; ++i) fill[i] = -1;
        fs.write(reinterpret_cast<char*>(fill), remaining);
        delete[] fill;
    }
    fs.flush();
}

void loadFreeListFromFS() {
    // clear in-memory free list
    FreeNode* cur = freeHead;
    while (cur) {
        FreeNode* nx = cur->next;
        delete cur;
        cur = nx;
    }
    freeHead = NULL;
    fs.seekg(DIR_REGION_SIZE, ios::beg);
    int entries = FREE_REGION_SIZE / sizeof(int);
    for (int i = 0; i < entries; ++i) {
        int val = -1;
        fs.read(reinterpret_cast<char*>(&val), sizeof(int));
        if (!fs) break;
        if (val == -1) break;
        // push onto freeHead
        FreeNode* n = new FreeNode(val);
        n->next = freeHead;
        freeHead = n;
    }
    // if file had no free list (fresh) -> initialize full free list
    if (!freeHead) {
        initFreeListAll();
    }
}

/* ---------------- Block read/write ---------------- */

void writeBlock(int blockNum, const char* data, int dataLen, int nextBlock) {
    if (blockNum < 0 || blockNum >= TOTAL_BLOCKS) return;
    int addr = getBlockAddress(blockNum);
    fs.seekp(addr, ios::beg);
    fs.write(reinterpret_cast<char*>(&nextBlock), sizeof(int));
    if (dataLen > 0) fs.write(data, dataLen);
    int pad = BLOCK_DATA_SIZE - dataLen;
    if (pad > 0) {
        char* zeros = new char[pad];
        memset(zeros, 0, pad);
        fs.write(zeros, pad);
        delete[] zeros;
    }
    fs.flush();
}

int readBlockData(int blockNum, char* outBuf, int bufSize) {
    // returns nextBlock; fills outBuf with up to bufSize bytes
    if (blockNum < 0 || blockNum >= TOTAL_BLOCKS) return -1;
    int addr = getBlockAddress(blockNum);
    fs.seekg(addr, ios::beg);
    int nextBlock = -1;
    fs.read(reinterpret_cast<char*>(&nextBlock), sizeof(int));
    int toRead = my_min(bufSize, BLOCK_DATA_SIZE);
    if (toRead > 0) fs.read(outBuf, toRead);
    return nextBlock;
}

/* ---------------- High level file operations ---------------- */

void listFiles() {
    if (!dirHead) { cout << "No files in system.\n"; return; }
    DirNode* cur = dirHead;
    int idx = 1;
    while (cur) {
        cout << idx++ << ". " << cur->filename << " (" << cur->fileSize << " bytes)\n";
        cur = cur->next;
    }
}

void viewFile() {
    cout << "Enter file name to view: ";
    string fname;
    cin >> fname;
    DirNode* f = findDirNodeMem(fname.c_str());
    if (!f) { cout << "File not found.\n"; return; }
    int remaining = f->fileSize;
    int cur = f->startBlock;
    cout << "----- Begin File -----\n";
    while (cur != -1 && remaining > 0) {
        int toRead = my_min(remaining, BLOCK_DATA_SIZE);
        char* buf = new char[toRead];
        int next = readBlockData(cur, buf, toRead);
        cout.write(buf, toRead);
        delete[] buf;
        remaining -= toRead;
        cur = next;
    }
    cout << "\n----- End File -----\n";
}

/* read full file into dynamically allocated buffer; caller must delete[] returned pointer */
char* readFullFileDataIntoBuffer(DirNode* f, int& outSize) {
    outSize = 0;
    if (!f) return NULL;
    int total = f->fileSize;
    if (total == 0) { outSize = 0; return NULL; }
    char* buffer = new char[total];
    int cur = f->startBlock;
    int written = 0;
    while (cur != -1 && written < total) {
        int toRead = my_min(BLOCK_DATA_SIZE, total - written);
        char* tmp = new char[toRead];
        int next = readBlockData(cur, tmp, toRead);
        memcpy(buffer + written, tmp, toRead);
        written += toRead;
        delete[] tmp;
        cur = next;
    }
    outSize = total;
    return buffer;
}

void createFileInteractive() {
    cout << "Enter file name (*.txt): ";
    string fname;
    cin >> fname;
    if (findDirNodeMem(fname.c_str())) { cout << "File already exists.\n"; return; }
    cout << "Enter data in this file:";
    cin.ignore();
    string data;
    getline(cin, data);
    int bytes = (int)data.size();
    if (bytes == 0) {
        addDirNodeMem(fname.c_str(), -1, 0);
        saveDirectoryToFS();
        saveFreeListToFS();
        cout << "Created empty file.\n";
        return;
    }
    int required = (bytes + BLOCK_DATA_SIZE - 1) / BLOCK_DATA_SIZE;
    if (required > countFreeBlocks()) { cout << "Not enough space. Required blocks: " << required << "\n"; return; }
    // allocate blocks into temporary array (max blocks allowed dynamically)
    int* allocated = new int[required];
    for (int i = 0; i < required; ++i) allocated[i] = -1;
    for (int i = 0; i < required; ++i) {
        int b = allocateBlock();
        if (b == -1) {
            // rollback
            for (int j = 0; j < i; ++j) deallocateBlock(allocated[j]);
            delete[] allocated;
            cout << "Allocation failed.\n"; return;
        }
        allocated[i] = b;
    }
    // write blocks
    for (int i = 0; i < required; ++i) {
        int startPos = i * BLOCK_DATA_SIZE;
        int len = my_min(BLOCK_DATA_SIZE, bytes - startPos);
        int next = (i + 1 < required) ? allocated[i + 1] : -1;
        writeBlock(allocated[i], data.c_str() + startPos, len, next);
    }
    addDirNodeMem(fname.c_str(), allocated[0], bytes);
    saveDirectoryToFS();
    saveFreeListToFS();
    delete[] allocated;
    cout << "File created. Used blocks: " << required << "\n";
}

void deleteFileInteractive() {
    cout << "Enter file name to delete: ";
    string fname;
    cin >> fname;
    DirNode* f = findDirNodeMem(fname.c_str());
    if (!f) { cout << "File not found.\n"; return; }
    int cur = f->startBlock;
    while (cur != -1) {
        int addr = getBlockAddress(cur);
        fs.seekg(addr, ios::beg);
        int next = -1;
        fs.read(reinterpret_cast<char*>(&next), sizeof(int));
        deallocateBlock(cur);
        cur = next;
    }
    deleteDirNodeMem(fname.c_str());
    saveDirectoryToFS();
    saveFreeListToFS();
    cout << "File deleted.\n";
}

void appendToFileInteractive() {
    cout << "Enter file name to append: ";
    string fname;
    cin >> fname;
    DirNode* f = findDirNodeMem(fname.c_str());
    if (!f) { cout << "File not found.\n"; return; }
    cout << "Enter data to append :\n";
    cin.ignore();
    string extra;
    getline(cin, extra);
    if (extra.empty()) { cout << "Nothing to append.\n"; return; }
    int extraLen = (int)extra.size();
    // find last block and used bytes in last block
    int lastBlock = -1;
    int cur = f->startBlock;
    if (cur == -1) {
        // file empty -> allocate one block
        if (freeHead == NULL) { cout << "No free blocks.\n"; return; }
        int nb = allocateBlock();
        f->startBlock = nb;
        lastBlock = nb;
    }
    else {
        int prev = -1;
        while (cur != -1) {
            prev = cur;
            fs.seekg(getBlockAddress(cur), ios::beg);
            int next;
            fs.read(reinterpret_cast<char*>(&next), sizeof(int));
            if (next == -1) { lastBlock = cur; break; }
            cur = next;
        }
    }
    // compute usedInLast
    int usedInLast = 0;
    if (f->fileSize == 0) usedInLast = 0;
    else {
        usedInLast = f->fileSize % BLOCK_DATA_SIZE;
        if (usedInLast == 0) usedInLast = BLOCK_DATA_SIZE;
    }
    int spaceInLast = BLOCK_DATA_SIZE - usedInLast;
    int writePos = 0;
    int toWrite = extraLen;
    // if lastBlock exists and has space
    if (spaceInLast > 0 && toWrite > 0) {
        int w = my_min(spaceInLast, toWrite);
        // read existing data of last block into temp
        char* tmpBuf = new char[BLOCK_DATA_SIZE];
        memset(tmpBuf, 0, BLOCK_DATA_SIZE);
        fs.seekg(getBlockAddress(lastBlock) + BLOCK_PTR_SIZE, ios::beg);
        if (usedInLast > 0) fs.read(tmpBuf, usedInLast);
        // append w bytes
        memcpy(tmpBuf + usedInLast, extra.c_str() + writePos, w);
        writePos += w; toWrite -= w;
        // preserve next pointer (-1)
        int nextPtr = -1;
        fs.seekp(getBlockAddress(lastBlock), ios::beg);
        fs.write(reinterpret_cast<char*>(&nextPtr), sizeof(int));
        fs.write(tmpBuf, BLOCK_DATA_SIZE);
        fs.flush();
        delete[] tmpBuf;
    }
    // allocate new blocks for remaining data
    // Count how many new blocks needed
    int remaining = toWrite;
    int needBlocks = (remaining > 0) ? ((remaining + BLOCK_DATA_SIZE - 1) / BLOCK_DATA_SIZE) : 0;
    if (needBlocks > 0 && needBlocks > countFreeBlocks()) {
        cout << "Not enough space to append full data. Appending partial if possible.\n";
    }
    // allocate and write sequentially
    int prevBlock = lastBlock;
    for (int i = 0; i < needBlocks; ++i) {
        int nb = allocateBlock();
        if (nb == -1) {
            cout << "Ran out of blocks during append. Partial append done.\n";
            break;
        }
        int w = my_min(BLOCK_DATA_SIZE, extraLen - writePos);
        int next = -1; // assume last for now; will be -1 unless next allocated
        // write initial next as -1 and data; will rewrite pointer of prevBlock to nb
        writeBlock(nb, extra.c_str() + writePos, w, -1);
        writePos += w;
        // update prevBlock's next pointer to nb
        if (prevBlock != -1) {
            fs.seekp(getBlockAddress(prevBlock), ios::beg);
            fs.write(reinterpret_cast<char*>(&nb), sizeof(int));
            fs.flush();
        }
        prevBlock = nb;
    }
    f->fileSize += writePos;
    saveDirectoryToFS();
    saveFreeListToFS();
    cout << "Append complete. New size: " << f->fileSize << " bytes.\n";
}

void copyFromWindowsInteractive() {
    cout << "Enter full source path (Windows) to import (e.g. C:\\path\\file.txt):\n";
    string path;
    cin.ignore();
    getline(cin, path);
    if (path.empty()) { cout << "Invalid path.\n"; return; }
    // extract filename
    int pos1 = (int)path.find_last_of('\\');
    int pos2 = (int)path.find_last_of('/');
    int pos = max(pos1, pos2);
    string fname = (pos == (int)string::npos) ? path : path.substr(pos + 1);
    if (fname.empty()) { cout << "Invalid file name.\n"; return; }
    if (findDirNodeMem(fname.c_str())) { cout << "File with same name exists.\n"; return; }
    ifstream src(path.c_str(), ios::binary);
    if (!src.is_open()) { cout << "Failed to open source file.\n"; return; }
    src.seekg(0, ios::end);
    long long sz = src.tellg();
    src.seekg(0, ios::beg);
    if (sz == 0) {
        addDirNodeMem(fname.c_str(), -1, 0);
        saveDirectoryToFS(); saveFreeListToFS();
        src.close();
        cout << "Imported empty file.\n"; return;
    }
    if (sz < 0) { cout << "Error getting file size.\n"; src.close(); return; }
    int bytes = (int)sz;
    char* buffer = new char[bytes];
    src.read(buffer, bytes);
    src.close();
    int required = (bytes + BLOCK_DATA_SIZE - 1) / BLOCK_DATA_SIZE;
    if (required > countFreeBlocks()) { cout << "Not enough space to import.\n"; delete[] buffer; return; }
    // allocate array to store allocated blocks
    int* allocated = new int[required];
    for (int i = 0; i < required; ++i) allocated[i] = -1;
    for (int i = 0; i < required; ++i) {
        int b = allocateBlock();
        if (b == -1) {
            for (int j = 0; j < i; ++j) deallocateBlock(allocated[j]);
            delete[] allocated; delete[] buffer;
            cout << "Allocation failed.\n"; return;
        }
        allocated[i] = b;
    }
    // write
    for (int i = 0; i < required; ++i) {
        int start = i * BLOCK_DATA_SIZE;
        int len = my_min(BLOCK_DATA_SIZE, bytes - start);
        int next = (i + 1 < required) ? allocated[i + 1] : -1;
        writeBlock(allocated[i], buffer + start, len, next);
    }
    addDirNodeMem(fname.c_str(), allocated[0], bytes);
    saveDirectoryToFS(); saveFreeListToFS();
    delete[] allocated; delete[] buffer;
    cout << "Imported file as: " << fname << " (" << bytes << " bytes)\n";
}

void copyToWindowsInteractive() {
    cout << "Enter file name in FS to export: ";
    string fname;
    cin >> fname;
    DirNode* f = findDirNodeMem(fname.c_str());
    if (!f) { cout << "File not found.\n"; return; }
    cout << "Enter destination full path including filename:\n";
    string dest;
    cin.ignore();
    getline(cin, dest);
    if (dest.empty()) { cout << "Invalid destination.\n"; return; }
    ofstream out(dest.c_str(), ios::binary);
    if (!out.is_open()) { cout << "Failed to open destination.\n"; return; }
    int remaining = f->fileSize;
    int cur = f->startBlock;
    while (cur != -1 && remaining > 0) {
        int toRead = my_min(remaining, BLOCK_DATA_SIZE);
        char* buf = new char[toRead];
        int next = readBlockData(cur, buf, toRead);
        out.write(buf, toRead);
        delete[] buf;
        remaining -= toRead;
        cur = next;
    }
    out.close();
    cout << "Exported to: " << dest << "\n";
}

/* Defragmentation: read all files into FileTemp linked list, clear data region,
   then write sequentially starting from block 0 upward. Update directory and free list */
void defragmentationInteractive() {
    cout << "Starting defragmentation...\n";
    // Build FileTemp linked list
    FileTemp* ftHead = NULL;
    FileTemp* ftTail = NULL;
    DirNode* cur = dirHead;
    while (cur) {
        FileTemp* ft = new FileTemp();
        strncpy_s(ft->filename, cur->filename, MAX_FILENAME_BYTES - 1);
        int size = 0;
        ft->data = readFullFileDataIntoBuffer(cur, size); // uses new[] or NULL
        ft->dataSize = size;
        ft->newStart = -1;
        ft->next = NULL;
        if (!ftHead) ftHead = ftTail = ft; else { ftTail->next = ft; ftTail = ft; }
        cur = cur->next;
    }
    // reset free list to full and zero data region
    FreeNode* fcur = freeHead;
    while (fcur) { FreeNode* nx = fcur->next; delete fcur; fcur = nx; }
    freeHead = NULL;
    for (int i = TOTAL_BLOCKS - 1; i >= 0; --i) {
        FreeNode* n = new FreeNode(i);
        n->next = freeHead; freeHead = n;
    }
    // zero data region for cleanliness
    char* zeros = new char[BLOCK_SIZE];
    memset(zeros, 0, BLOCK_SIZE);
    for (int b = 0; b < TOTAL_BLOCKS; ++b) {
        fs.seekp(getBlockAddress(b), ios::beg);
        fs.write(zeros, BLOCK_SIZE);
    }
    fs.flush();
    delete[] zeros;
    // Now sequentially write each file from ftHead
    int nextFreeIndex = 0; // we will pop from freeHead using allocateBlock()
    FileTemp* ftn = ftHead;
    while (ftn) {
        if (ftn->dataSize == 0) {
            ftn->newStart = -1;
            ftn = ftn->next;
            continue;
        }
        int bytes = ftn->dataSize;
        int required = (bytes + BLOCK_DATA_SIZE - 1) / BLOCK_DATA_SIZE;
        // allocate blocks array
        int* allocated = new int[required];
        for (int i = 0; i < required; ++i) allocated[i] = -1;
        for (int i = 0; i < required; ++i) {
            int b = allocateBlock();
            if (b == -1) {
                // Shouldn't happen here; but if happens, cleanup and abort
                cout << "Defrag error: insufficient space.\n";
                for (int j = 0; j < i; ++j) deallocateBlock(allocated[j]);
                delete[] allocated;
                // cleanup temps
                FileTemp* t = ftHead;
                while (t) { if (t->data) { delete[] t->data; t->data = NULL; } FileTemp* nx = t->next; delete t; t = nx; }
                return;
            }
            allocated[i] = b;
        }
        // write blocks
        for (int i = 0; i < required; ++i) {
            int start = i * BLOCK_DATA_SIZE;
            int len = my_min(BLOCK_DATA_SIZE, bytes - start);
            int next = (i + 1 < required) ? allocated[i + 1] : -1;
            writeBlock(allocated[i], ftn->data + start, len, next);
        }
        ftn->newStart = allocated[0];
        delete[] allocated;
        ftn = ftn->next;
    }
    // update directory nodes in memory by matching names (preserve order)
    DirNode* dcur = dirHead;
    FileTemp* fcurt = ftHead;
    while (dcur && fcurt) {
        dcur->startBlock = fcurt->newStart;
        dcur->fileSize = fcurt->dataSize;
        dcur = dcur->next;
        fcurt = fcurt->next;
    }
    // cleanup temps
    fcurt = ftHead;
    while (fcurt) {
        if (fcurt->data) delete[] fcurt->data;
        FileTemp* nx = fcurt->next;
        delete fcurt;
        fcurt = nx;
    }
    // persist directory and free list
    saveDirectoryToFS();
    saveFreeListToFS();
    cout << "Defragmentation complete.\n";
}

/* ---------------- Menu & main ---------------- */

void showMenu() {
    cout << "\n===== FILE SYSTEM MENU =====\n";
    cout << "1. Create New File\n";
    cout << "2. List Files\n";
    cout << "3. View File\n";
    cout << "4. Delete File\n";
    cout << "5. Append to File (Modify)\n";
    cout << "6. Copy file FROM Windows (import)\n";
    cout << "7. Copy file TO Windows (export)\n";
    cout << "8. Defragmentation:\n";
    cout << "9. Exit\n";
    cout << "Enter choice: ";
}

void createEmptyFSFile() {
    ofstream create(FS_FILENAME.c_str(), ios::binary);
    if (!create) { cerr << "Cannot create file system file.\n"; exit(1); }
    const int chunk = 8192;
    char* zeros = new char[chunk];
    memset(zeros, 0, chunk);
    int remaining = TOTAL_SIZE;
    while (remaining > 0) {
        int w = my_min(remaining, chunk);
        create.write(zeros, w);
        remaining -= w;
    }
    delete[] zeros;
    create.close();
}

void initFileSystem() {
    ifstream check(FS_FILENAME.c_str(), ios::binary);
    if (!check.good()) {
        cout << "Creating new file system file...\n";
        createEmptyFSFile();
        // open and initialize free list region
        fs.open(FS_FILENAME.c_str(), ios::in | ios::out | ios::binary);
        if (!fs.is_open()) { cerr << "Failed to open fs file.\n"; exit(1); }
        // init full free list
        initFreeListAll();
        saveFreeListToFS();
        // directory region already zeros
        fs.close();
    }
    else {
        check.close();
    }
    fs.open(FS_FILENAME.c_str(), ios::in | ios::out | ios::binary);
    if (!fs.is_open()) { cerr << "Failed to open file system.\n"; exit(1); }
    // load directory and free list
    loadDirectoryFromFS();
    loadFreeListFromFS();
}

int main() {
    cout << "Initializing file system...\n";
    initFileSystem();
    cout << "Loaded. Free blocks available: " << countFreeBlocks() << "\n";
    while (true) {
        showMenu();
        int choice;
        if (!(cin >> choice)) { cin.clear(); string dum; getline(cin, dum); cout << "Invalid input.\n"; continue; }
        switch (choice) {
        case 1: createFileInteractive(); break;
        case 2: listFiles(); break;
        case 3: viewFile(); break;
        case 4: deleteFileInteractive(); break;
        case 5: appendToFileInteractive(); break;
        case 6: copyFromWindowsInteractive(); break;
        case 7: copyToWindowsInteractive(); break;
        case 8: defragmentationInteractive(); break;
        case 9:
            cout << "Saving and exiting...\n";
            saveDirectoryToFS();
            saveFreeListToFS();
            fs.close();
            return 0;
        default: cout << "Invalid choice.\n";
        }
        cout << "Free blocks left: " << countFreeBlocks() << "\n";
    }
    return 0;
}
/*C:\Users\SMART TECH\Documents\export.txt*/
