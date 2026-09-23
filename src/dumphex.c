#include "dumphex.h"
#include "printf.h"

/*
 * 16 bytes/row hex + ASCII dump, e.g.:
 *   0x40000000 | 00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  |  ........ \r\n
 * The last row is padded with blanks so the "|  ascii" column always
 * lines up, whether or not `size` is a multiple of 16.
 */
void DumpHex(const void *data, uint32_t size)
{
	const unsigned char *bytes = (const unsigned char *)data;
	uint32_t offset;

	for (offset = 0; offset < size; offset += 16) {
		uint32_t row_len = (size - offset < 16) ? (size - offset) : 16;
		char ascii[17];
		uint32_t col;

		printf("0x%08X | ", (unsigned int)(MPTRS)(bytes + offset));

		for (col = 0; col < 16; ++col) {
			if (col < row_len) {
				unsigned char b = bytes[offset + col];
				printf("%02X ", b);
				ascii[col] = (b >= ' ' && b <= '~') ? (char)b : '.';
			} else {
				printf("   "); // pad missing columns in a short last row
			}
			if (col == 7)
				printf(" "); // gap between the two 8-byte halves
		}
		ascii[row_len] = '\0';

		printf(" |  %s \r\n", ascii);
	}
}
