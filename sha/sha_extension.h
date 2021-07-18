#include <stdbool.h>
#include <stdint.h>

extern bool hasShaExtension;
bool checkShaExtension();

// uses intel sha intrinsics; only available on certain cpus
void sha256_process_x86(uint32_t state[8], const uint8_t data[], uint32_t length);
