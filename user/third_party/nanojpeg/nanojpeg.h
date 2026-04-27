/*
 * Public API of NanoJPEG.  Mirrors the prototypes in nanojpeg.c so
 * callers do not need to use the dual-mode "include the .c file"
 * pattern.  The implementation lives in nanojpeg.c, vendored
 * verbatim under the original KeyJ Research License.
 */

#ifndef NANOJPEG_H
#define NANOJPEG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	NJ_OK = 0,
	NJ_NO_JPEG,
	NJ_UNSUPPORTED,
	NJ_OUT_OF_MEM,
	NJ_INTERNAL_ERR,
	NJ_SYNTAX_ERROR,
	__NJ_FINISHED__,
} nj_result_t;

void njInit(void);
nj_result_t njDecode(const void *jpeg, const int size);
int njGetWidth(void);
int njGetHeight(void);
int njIsColor(void);
unsigned char *njGetImage(void);
int njGetImageSize(void);
void njDone(void);

#ifdef __cplusplus
}
#endif

#endif
