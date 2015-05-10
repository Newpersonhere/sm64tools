#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libmio0.h"
#include "libsm64.h"
#include "utils.h"

// TODO: make these configurable
#define IN_START_ADDR  0x00100000
#define OUT_START_ADDR 0x00800000

// extract opcode from instruction MSB
#define OPCODE(VAL_) ((VAL_) & 0xFC)

typedef struct
{
   unsigned int old;      // MIO0 address in original ROM
   unsigned int new;      // starting MIO0 address in extended ROM
   unsigned int new_end;  // ending MIO0 address in extended ROM
   unsigned int addr;     // ASM address for referenced pointer
   unsigned char command; // command type: 0x1A or 0x18 (or 0xFF for ASM)
} ptr_t;

// compare function for sorting pointers
static int cmp_ptr(const void *a, const void *b)
{
   const ptr_t *pa = a;
   const ptr_t *pb = b;
   return (pa->old - pb->old);
}

// find a pointer in the list and return index
// ptr: address to find in table old values
// table: list of addresses to MIO0 data
// count: number of addresses in table
// returns index in table if found, -1 otherwise
static int find_ptr(unsigned int ptr, ptr_t table[], int count)
{
   int i;
   for (i = 0; i < count; i++) {
      if (ptr == table[i].old) {
         return i;
      }
   }
   return -1;
}

// find locations of existing MIO0 data
// buf: buffer containing SM64 data
// length: length of buf
// table: table to store MIO0 addresses in
// returns number of MIO0 files stored in table old values
static int find_mio0(unsigned char *buf, unsigned int length, ptr_t table[])
{
   unsigned int addr;
   int count = 0;

   // MIO0 data is on 16-byte boundaries
   for (addr = IN_START_ADDR; addr < length; addr += 16) {
      if (!memcmp(&buf[addr], "MIO0", 4)) {
         table[count].old = addr;
         count++;
      }
   }
   return count;
}

// find pointers to MIO0 files and stores command type
// buf: buffer containing SM64 data
// length: length of buf
// table: list of addresses to MIO0 data
// count: number of addresses in table
static void find_pointers(unsigned char *buf, unsigned int length, ptr_t table[], int count)
{
   unsigned int addr;
   unsigned int ptr;
   int idx;

   for (addr = IN_START_ADDR; addr < length; addr += 4) {
      if ((buf[addr] == 0x18 || buf[addr] == 0x1A) && buf[addr+1] == 0x0C && buf[addr+2] == 0x00) {
         ptr = read_u32_be(&buf[addr+4]);
         idx = find_ptr(ptr, table, count);
         if (idx >= 0) {
            table[idx].command = buf[addr];
         }
      }
   }
}

// find references to the MIO0 blocks in ASM and store type
// buf: buffer containing SM64 data
// length: length of buf
// table: list of addresses to MIO0 data
// count: number of addresses in table
static void find_asm_pointers(unsigned char *buf, ptr_t table[], int count)
{
   // find the ASM references
   // looking for some code that looks like this:
   // LUI     a1,start_upper    ; start ptr MSword
   // LUI     a2,end_upper      ; end ptr MSword
   // ADDIU   a2,a2,end_lower   ; end ptr LSword
   // ADDIU   a1,a1,start_lower ; start ptr LSword
   // JAL     somewhere
   unsigned int addr;
   unsigned int ptr;
   unsigned int end;
   int idx;
   unsigned short addr_low, addr_high;
   for (addr = 0; addr < IN_START_ADDR; addr += 4) {
      if (OPCODE(buf[addr])   == 0x3C && OPCODE(buf[addr+4])  == 0x3C &&
          OPCODE(buf[addr+8]) == 0x24 && OPCODE(buf[addr+12]) == 0x24) {
         // reconstruct address
         addr_high = read_u16_be(&buf[addr + 0x2]);
         addr_low = read_u16_be(&buf[addr + 0xe]);
         // ADDIU sign extends which causes the encoded high val to be +1 if low MSb is set
         if (addr_low & 0x8000) {
            addr_high--;
         }
         ptr = (addr_high << 16) | addr_low;
         // reconstruct end address
         addr_high = read_u16_be(&buf[addr + 0x6]);
         addr_low = read_u16_be(&buf[addr + 0xa]);
         // ADDIU sign extends which causes the encoded high val to be +1 if low MSb is set
         if (addr_low & 0x8000) {
            addr_high--;
         }
         end = (addr_high << 16) | addr_low;
         idx = find_ptr(ptr, table, count);
         if (idx >= 0) {
            INFO("Found ASM reference to %X at %X\n", ptr, addr);
            table[idx].command = 0xFF;
            table[idx].addr = addr;
            table[idx].new_end = end;
         }
      }
   }
}

// find pointers to MIO0 blocks in extended area and store command type
// buf: buffer containing SM64 data
// length: length of buf
// table: list of addresses to MIO0 data
// returns number of MIO0 files stored in table old values
static int find_ext_pointers(unsigned char *buf, unsigned int length, ptr_t table[])
{
   unsigned int addr;
   unsigned int ptr;
   int count;
   int idx;

   count = 0;
   for (addr = IN_START_ADDR; addr < length; addr += 4) {
      if ((buf[addr] == 0x17 || buf[addr] == 0x18 || buf[addr] == 0x1A)
            && buf[addr+1] == 0x0C && buf[addr+2] < 0x02) {
         ptr = read_u32_be(&buf[addr+4]);
         if (ptr >= OUT_START_ADDR && ptr < length) {
            idx = find_ptr(ptr, table, count);
            if (idx < 0) {
               table[count].old = ptr;
               table[count].new_end = read_u32_be(&buf[addr+8]);
               if (table[count].new_end < length && table[count].new_end > table[count].old) {
                  table[count].command = buf[addr];
                  count++;
               }
            }
         }
      }
   }
   return count;
}

// adjust pointers to from old to new locations
// buf: buffer containing SM64 data
// length: length of buf
// table: list of addresses to MIO0 data
// count: number of addresses in table
static void sm64_adjust_pointers(unsigned char *buf, unsigned int length, ptr_t table[], int count)
{
   unsigned int addr;
   unsigned int old_ptr;
   int idx;
   for (addr = IN_START_ADDR; addr < length; addr += 4) {
      if ((buf[addr] == 0x17 || buf[addr] == 0x18 || buf[addr] == 0x1A) && buf[addr+1] == 0x0C && buf[addr+2] < 0x02) {
         old_ptr = read_u32_be(&buf[addr+4]);
         idx = find_ptr(old_ptr, table, count);
         if (idx >= 0) {
            INFO("Old pointer at %X = ", addr);
            INFO_HEX(&buf[addr], 12);
            INFO("\n");
            write_u32_be(&buf[addr+4], table[idx].new);
            write_u32_be(&buf[addr+8], table[idx].new_end);
            if (buf[addr] != table[idx].command) {
               buf[addr] = table[idx].command;
            }
            INFO("NEW pointer at %X = ", addr);
            INFO_HEX(&buf[addr], 12);
            INFO("\n");
         }
      }
   }
}

// adjust 'pointer' encoded in ASM LUI and ADDIU instructions
static void sm64_adjust_asm(unsigned char *buf, ptr_t table[], int count)
{
   unsigned int addr;
   int i;
   unsigned short addr_low, addr_high;
   for (i = 0; i < count; i++) {
      if (table[i].command == 0xFF) {
         addr = table[i].addr;
         INFO("Old ASM reference at %X = ", addr);
         INFO_HEX(&buf[addr], 16);
         INFO("\n");
         addr_low = table[i].new & 0xFFFF;
         addr_high = (table[i].new >> 16) & 0xFFFF;
         // ADDIU sign extends which causes the summed high to be 1 less if low MSb is set
         if (addr_low & 0x8000) {
            addr_high++;
         }
         write_u16_be(&buf[addr + 0x2], addr_high);
         write_u16_be(&buf[addr + 0xe], addr_low);

         addr_low = table[i].new_end & 0xFFFF;
         addr_high = (table[i].new_end >> 16) & 0xFFFF;
         if (addr_low & 0x8000) {
            addr_high++;
         }
         write_u16_be(&buf[addr + 0x6], addr_high);
         write_u16_be(&buf[addr + 0xa], addr_low);
         INFO("NEW ASM reference at %X = ", addr);
         INFO_HEX(&buf[addr], 16);
         INFO(" [%06X - %06X]\n", table[i].new, table[i].new_end);
      }
   }
}

// compute N64 ROM checksums
// buf: buffer with extended SM64 data
// cksum: two element array to write CRC1 and CRC2 to
// TODO: this could be hand optimized
static void sm64_calc_checksums(unsigned char *buf, unsigned int cksum[]) {
   unsigned int t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;
   unsigned int s0, s6;
   unsigned int a0, a1, a2, a3, at;
   unsigned int lo;
   unsigned int v0, v1;
   unsigned int ra;

   // derived from the SM64 boot code
   s6 = 0x3f;
   a0 = 0x1000;     // 59c:   8d640008    lw a0,8(t3)
   a1 = s6;         // 5a0:   02c02825    move  a1,s6
   at = 0x5d588b65; // 5a4:   3c015d58    lui   at,0x5d58
                    // 5a8:   34218b65    ori   at,at,0x8b65
   lo = a1 * at;    // 5ac:   00a10019    multu a1,at    16 F8CA 4DDB

   ra = 0x100000; // 5bc:  3c1f0010    lui   ra,0x10
   v1 = 0;  // 5c0:  00001825    move  v1,zero
   t0 = 0;  // 5c4:  00004025    move  t0,zero
   t1 = a0; // 5c8:  00804825    move  t1,a0
   t5 = 32; // 5cc:  240d0020    li t5,32
   v0 = lo; // 5d0:  00001012    mflo  v0
   v0++;    // 5d4:  24420001    addiu v0,v0,1
   a3 = v0; // 5d8:  00403825    move  a3,v0
   t2 = v0; // 5dc:  00405025    move  t2,v0
   t3 = v0; // 5e0:  00405825    move  t3,v0
   s0 = v0; // 5e4:  00408025    move  s0,v0
   a2 = v0; // 5e8:  00403025    move  a2,v0
   t4 = v0; // 5ec:  00406025    move  t4,v0

   do {
      v0 = read_u32_be(&buf[t1]);   // 5f0: 8d220000    lw v0,0(t1)
      v1 = a3 + v0;   // 5f4: 00e21821    addu  v1,a3,v0
      at = (v1 < a3); // 5f8: 0067082b    sltu  at,v1,a3
      a1 = v1;        // 600: 00602825    move  a1,v1 branch delay slot
      if (at) {       // 5fc: 10200002    beqz  at,0x608
         t2++;        // 604: 254a0001    addiu t2,t2,1
      }
      v1 = v0 & 0x1F;  // 608: 3043001f    andi  v1,v0,0x1f
      t7 = t5 - v1;    // 60c: 01a37823    subu  t7,t5,v1
      t8 = v0 >> t7;   // 610: 01e2c006    srlv  t8,v0,t7
      t6 = v0 << v1;   // 614: 00627004    sllv  t6,v0,v1
      a0 = t6 | t8;    // 618: 01d82025    or a0,t6,t8
      at = (a2 < v0);  // 61c: 00c2082b    sltu  at,a2,v0
      a3 = a1;         // 620: 00a03825    move  a3,a1
      t3 ^= v0;        // 624: 01625826    xor   t3,t3,v0
      s0 += a0;        // 62c: 02048021    addu  s0,s0,a0 branch delay slot
      if (at) {        // 628: 10200004    beqz  at,0x63c
         t9 = a3 ^ v0; // 630: 00e2c826    xor   t9,a3,v0
                       // 634: 10000002    b  0x640
         a2 ^= t9;     // 638: 03263026    xor   a2,t9,a2 branch delay
      } else {
         a2 ^= a0;     // 63c: 00c43026    xor   a2,a2,a0
      }
      t0 += 4;         // 640: 25080004    addiu t0,t0,4
      t7 = v0 ^ s0;    // 644: 00507826    xor   t7,v0,s0
      t1 += 4;         // 648: 25290004    addiu t1,t1,4
      t4 += t7;        // 650: 01ec6021    addu  t4,t7,t4 branch delay
   } while (t0 != ra); // 64c: 151fffe8    bne   t0,ra,0x5f0
   t6 = a3 ^ t2;       // 654: 00ea7026    xor   t6,a3,t2
   a3 = t6 ^ t3;       // 658: 01cb3826    xor   a3,t6,t3
   t8 = s0 ^ a2;       // 65c: 0206c026    xor   t8,s0,a2
   s0 = t8 ^ t4;       // 660: 030c8026    xor   s0,t8,t4
   
   cksum[0] = a3;
   cksum[1] = s0;
}

int sm64_rom_type(unsigned char *buf, unsigned int length)
{
   const unsigned char bs[] = {0x37, 0x80, 0x40, 0x12};
   const unsigned char be[] = {0x80, 0x37, 0x12, 0x40};
   if (!memcmp(buf, bs, sizeof(bs)) && length == (8*MB)) {
      return 1; // byte-swapped
   }
   if (!memcmp(buf, be, sizeof(be))) {
      if (length == 8*MB) {
         return 2; // big-endian
      } else if (length > 8*MB) {
         return 0; // already extended
      }
   }
   return -1; // invalid
}

void sm64_decompress_mio0(const sm64_config_t *config,
                          unsigned char *in_buf,
                          unsigned int in_length,
                          unsigned char *out_buf)
{
#define MAX_PTRS 128
#define COMPRESSED_LENGTH 2
   mio0_header_t head;
   int bit_length;
   int move_offset;
   unsigned int in_addr;
   unsigned int out_addr = OUT_START_ADDR;
   unsigned int align_add = config->alignment - 1;
   unsigned int align_mask = ~align_add;
   ptr_t ptr_table[MAX_PTRS];
   int ptr_count;
   int i;

   // find MIO0 locations and pointers
   ptr_count = find_mio0(in_buf, in_length, ptr_table);
   find_pointers(in_buf, in_length, ptr_table, ptr_count);
   find_asm_pointers(in_buf, ptr_table, ptr_count);

   // extract each MIO0 block and prepend fake MIO0 header for 0x1A command and ASM references
   for (i = 0; i < ptr_count; i++) {
      in_addr = ptr_table[i].old;
      if (!memcmp(&in_buf[in_addr], "MIO0", 4)) {
         unsigned int end;
         int length;
         int is_mio0 = 0;
         // align output address
         out_addr = (out_addr + align_add) & align_mask;
         length = mio0_decode(&in_buf[in_addr], &out_buf[out_addr], &end);
         if (length > 0) {
            // dump MIO0 data and decompressed data to file
            if (config->dump) {
               char filename[FILENAME_MAX];
               sprintf(filename, MIO0_DIR "/%08X.mio", in_addr);
               write_file(filename, &in_buf[in_addr], end);
               sprintf(filename, MIO0_DIR "/%08X", in_addr);
               write_file(filename, &out_buf[out_addr], length);
            }
            // 0x1A commands and ASM references need fake MIO0 header
            // relocate data and add MIO0 header with all uncompressed data
            if (ptr_table[i].command == 0x1A || ptr_table[i].command == 0xFF) {
               bit_length = (length + 7) / 8 + 2;
               move_offset = MIO0_HEADER_LENGTH + bit_length + COMPRESSED_LENGTH;
               memmove(&out_buf[out_addr + move_offset], &out_buf[out_addr], length);
               head.dest_size = length;
               head.comp_offset = move_offset - COMPRESSED_LENGTH;
               head.uncomp_offset = move_offset;
               mio0_encode_header(&out_buf[out_addr], &head);
               memset(&out_buf[out_addr + MIO0_HEADER_LENGTH], 0xFF, head.comp_offset - MIO0_HEADER_LENGTH);
               memset(&out_buf[out_addr + head.comp_offset], 0x0, 2);
               length += head.uncomp_offset;
               is_mio0 = 1;
            } else if (ptr_table[i].command == 0x18) {
               // 0x18 commands become 0x17
               ptr_table[i].command = 0x17;
            }
            INFO("MIO0 file from %08X is decompressed at %08X to %08X as raw data%s\n",
                  in_addr, out_addr, out_addr + length, is_mio0 ? " with a MIO0 header" : "");
            if (config->fill) {
               INFO("Filling old MIO0 with 0x01 from %X length %X\n", in_addr, end);
               memset(&out_buf[in_addr], 0x01, end);
            }
            // keep track of new pointers
            ptr_table[i].new = out_addr;
            ptr_table[i].new_end = out_addr + length;
            out_addr += length + config->padding;
         } else {
            ERROR("Error decoding MIO0 block at %X\n", in_addr);
         }
      }
   }

   // adjust pointers and ASM pointers to new values
   sm64_adjust_pointers(out_buf, in_length, ptr_table, ptr_count);
   sm64_adjust_asm(out_buf, ptr_table, ptr_count);
}

int sm64_compress_mio0(const sm64_config_t *config,
                       unsigned char *in_buf,
                       unsigned int in_length,
                       unsigned char *out_buf)
{
   ptr_t ptr_table[MAX_PTRS];
   unsigned int out_addr;
   int ptr_count;
   int out_length;
   int i;

   memset(ptr_table, 0, sizeof(ptr_table));
   ptr_count = 1 + find_ext_pointers(in_buf, in_length, &ptr_table[1]);
   // hard code ASM pointer
   ptr_table[0].old = OUT_START_ADDR;
   find_asm_pointers(in_buf, ptr_table, ptr_count);

   // sort the addresses
   qsort(ptr_table, ptr_count, sizeof(ptr_table[0]), cmp_ptr);

   // debug table
   for (i = 0; i < ptr_count; i++) {
      INFO("%02X %8X %8X %6X\n", ptr_table[i].command, ptr_table[i].old, ptr_table[i].new_end, ptr_table[i].new_end - ptr_table[i].old);
   }

   out_addr = OUT_START_ADDR;
   for (i = 0; i < ptr_count; i++) {
      unsigned int in_addr = ptr_table[i].old;
      unsigned int length = ptr_table[i].new_end - in_addr;
      unsigned int comp_length = 0;
      out_addr = ALIGN(in_addr, 16);
      // overwrite the old data in the output buffer
      memset(&out_buf[in_addr], 0x01, length);
      switch (ptr_table[i].command) {
         case 0x17: // 0x17 commands have raw data
            if (config->compress) {
               ptr_table[i].command = 0x18;
               INFO("Compressing 0x17 from %08X to %08X\n", in_addr, out_addr);
               comp_length = mio0_encode(&in_buf[in_addr], length, &out_buf[out_addr]);
            } else {
               INFO("Copying 0x17 from %08X to %08X (%X)\n", in_addr, out_addr, length);
               memcpy(&out_buf[out_addr], &in_buf[in_addr], length);
               comp_length = length;
            }
            break;
         case 0x18: // 0x18 commands have real MIO0 headers
            INFO("Copying 0x18 from %08X to %08X\n", in_addr, out_addr);
            memcpy(&out_buf[out_addr], &in_buf[in_addr], length);
            comp_length = length;
            break;
         case 0x1A: // 0x1A commands have fake MIO0 header
         case 0xFF: // so do ASM references
            if (config->compress) {
               mio0_header_t head;
               mio0_decode_header(&in_buf[in_addr], &head);
               in_addr += head.uncomp_offset;
               INFO("Compressing 0x%02X from %08X to %08X\n", ptr_table[i].command, in_addr, out_addr);
               comp_length = mio0_encode(&in_buf[in_addr], head.dest_size, &out_buf[out_addr]);
            } else {
               INFO("Copying 0x%02X from %08X to %08X (%X)\n", ptr_table[i].command, in_addr, out_addr, length);
               memcpy(&out_buf[out_addr], &in_buf[in_addr], length);
               comp_length = length;
            }
            break;
         default:
            ERROR("Error: what is command %02X\n", ptr_table[i].command);
            break;
      }
      ptr_table[i].new = out_addr;
      ptr_table[i].new_end = out_addr + comp_length;
   }

   // adjust pointers and ASM pointers to new values
   sm64_adjust_pointers(out_buf, in_length, ptr_table, ptr_count);
   sm64_adjust_asm(out_buf, ptr_table, ptr_count);

   // detect audio patch and fix it
   if (out_buf[0xd48b6] == 0x80 && out_buf[0xd48b7] == 0x3D) {
      INFO("Moving sound allocation from 0x803D0000 to 0x807B0000\n");
      out_buf[0xd48b7] = 0x7B;
   }

   out_length = in_length;

   return out_length;
}

void sm64_update_checksums(unsigned char *buf)
{
   unsigned int cksum_offsets[] = {0x10, 0x14};
   unsigned int read_cksum[2];
   unsigned int calc_cksum[2];
   int i;

   // assume CIC-NUS-6102
   INFO("BootChip: CIC-NUS-6102\n");

   // calculate new N64 header checksum
   sm64_calc_checksums(buf, calc_cksum);

   // mimic the n64sums output
   for (i = 0; i < 2; i++) {
      read_cksum[i] = read_u32_be(&buf[cksum_offsets[i]]);
      INFO("CRC%d: 0x%08X ", i+1, read_cksum[i]);
      INFO("Calculated: 0x%08X ", calc_cksum[i]);
      if (calc_cksum[i] == read_cksum[i]) {
         INFO("(Good)\n");
      } else {
         INFO("(Bad)\n");
      }
   }

   // write checksums into header
   INFO("Writing back calculated Checksum\n");
   write_u32_be(&buf[cksum_offsets[0]], calc_cksum[0]);
   write_u32_be(&buf[cksum_offsets[1]], calc_cksum[1]);
}
