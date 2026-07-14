//
// Created by SyncShard on 7/6/26.
//

#include "sync_malloc.h"


#include <stdio.h>
#include <string.h>

const char *test_str = "Hello world!\n";
const char *test_str2 = "beep beep!\n";
const char *test3 = "beep";

#define ITERS 1'000'000


int main(void)
{
	void *aligned_str = NULL;
	void *aligned_str2 = NULL;

	int err = posix_memalign(&aligned_str, 0x40, 0x80);
	int err2 = posix_memalign(&aligned_str2, 0x80, 0x100);

	memcpy(aligned_str, test_str2, strlen(test_str2) + 1);
	printf(aligned_str);

	void *tmp = realloc(aligned_str, 0x100);
	if (!tmp) {
		free(aligned_str);
		free(aligned_str2);
		return 1;
	}
	aligned_str = tmp;

	memcpy(aligned_str, test_str, strlen(test_str) + 1);
	memcpy(aligned_str2, test_str2, strlen(test_str2) + 1);
	printf(aligned_str);

	free(aligned_str);
	free(aligned_str2);

	char *str = malloc(0x80);
	memcpy(str, test_str, strlen(test_str));
	printf(str);

	char *str2 = malloc(0x41000);
	for (int i = 0; i < ITERS; i++) {
		str = realloc(str, 0x40);
		memcpy(str, test_str2, strlen(test_str2));

		str = realloc(str, 0x80);
		memcpy(str, test_str, strlen(test_str));
		free(str);

		str = malloc(0x40);
		memcpy(str, test_str2, strlen(test_str2));

		str = realloc(str, 0x80);
		memcpy(str, test_str, strlen(test_str));
		free(str);
		str = NULL;
	}

	str = malloc(0x40000);
	for (size_t i = 0; i < 0x10 * strlen(test3); i += strlen(test3)) {
		memcpy(str + i, test3, strlen(test3));
	}
	fprintf(stdout, str);
	free(str);
	free(str2);
}
