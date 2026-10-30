#pragma once
/*
 * storage.h - the small API that storage.c offers to cli.c. Owner: Ariko.
 *
 * cli_task must NOT open files on the SD card itself (locking rule 3 in
 * shared.h). Instead it asks storage_task to do the reading.
 */
#include <stdbool.h>

/* Ask storage_task to print the last n log records to the console and wait
 * until it has finished. Returns false if storage_task is not running yet. */
bool storage_print_last(int n);