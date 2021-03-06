/*
  sdcard.c - An embedded CNC Controller with rs274/ngc (g-code) support

  Driver code for Texas Instruments Tiva C (TM4C123GH6PM) ARM processor

  Part of Grbl

  Copyright (c) 2018 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>

#include "driver.h"
#include "sdcard.h"
#include "GRBL/grbl.h"

// https://e2e.ti.com/support/tools/ccs/f/81/t/428524?Linking-error-unresolved-symbols-rom-h-pinout-c-

/* uses fatfs - http://www.elm-chan.org/fsw/ff/00index_e.html */

#ifdef SDCARD_SUPPORT

#define MAX_PATHLEN 128
#define LCAPS(c) ((c >= 'A' && c <= 'Z') ? c | 0x20 : c)

#include "fatfs/ff.h"
#include "fatfs/diskio.h"

char const *const filetypes[] = {
    "nc",
    "gcode",
    "txt",
    "text",
    "tap",
    "ngc",
    ""
};

static FIL cncfile;

typedef enum {
    Filename_Filtered = 0,
    Filename_Valid,
    Filename_Invalid
} file_status_t;

typedef struct
{
    FATFS *fs;
    FIL *handle;
    size_t size;
    size_t pos;
    uint32_t line;
    uint8_t eol;
} file_t;

static file_t file = {
    .fs = NULL,
    .handle = NULL,
    .size = 0,
	.line = 0,
    .pos = 0
};

static int16_t (*readfn)(void) = NULL;
bool (*suspend_readfn)(bool await) = NULL;;

static file_status_t allowed (char *filename, bool is_file)
{
    uint_fast8_t idx = 0;
    char filetype[8], *ftptr;
    file_status_t status = is_file ? Filename_Filtered : Filename_Valid;

    if(is_file && (ftptr = strrchr(filename, '.'))) {
        ftptr++;
        if(strlen(ftptr) > sizeof(filetype) - 1)
            return status;
        while(ftptr[idx]) {
            filetype[idx] = LCAPS(ftptr[idx]);
            idx++;
        }
        filetype[idx] = '\0';
        idx = 0;
        while(status == Filename_Filtered && filetypes[idx][0]) {
            if(!strcmp(filetype, filetypes[idx]))
                status = Filename_Valid;
            idx++;;
        }
    }

    if(status == Filename_Valid) {
        if(strchr(filename, ' ') ||
            strchr(filename, CMD_STATUS_REPORT) ||
             strchr(filename, CMD_CYCLE_START) ||
              strchr(filename, CMD_FEED_HOLD))
            status = Filename_Invalid;
    //TODO: check for top bit set characters
    }

    return status;
}

static inline char *get_name (FILINFO *file)
{
    return *file->lfname == '\0' ? file->fname : file->lfname;
}

static FRESULT scan_dir (char *path, uint_fast8_t depth, char *buf)
{
    DIR dir;
    FILINFO fno;
    FRESULT res;
    file_status_t status;
#if _USE_LFN
    static TCHAR lfn[_MAX_LFN + 1];   /* Buffer to store the LFN */
    fno.lfname = lfn;
    fno.lfsize = sizeof(lfn);
#endif

   if((res = f_opendir(&dir, path)) != FR_OK)
        return res;

    while(true) {

        if((res = f_readdir(&dir, &fno)) != FR_OK || fno.fname[0] == '\0')
            break;

        if(fno.fattrib & AM_DIR) { // It is a directory
            if((status = allowed(get_name(&fno), false)) == Filename_Valid) {
                if(strlen(path) + strlen(get_name(&fno)) > (MAX_PATHLEN - 1))
                    break;
                sprintf(&path[strlen(path)], "/%s", get_name(&fno));
                if(!(--depth && (res = scan_dir(path, depth, buf)) == FR_OK))
                    break;
            }
        } else if((status = allowed(get_name(&fno), true)) != Filename_Filtered) { // It is a file
            sprintf(buf, "[FILE:%s/%s|SIZE:%lu%s]\r\n", path, get_name(&fno), fno.fsize, status == Filename_Invalid ? "|UNUSABLE" : "");
            hal.stream_write(buf);
        }
    }
//    f_closedir(&dir); // requires never version?

    return res;
}

static void file_close (void)
{
    if(file.handle) {
        f_close(file.handle);
        file.handle = NULL;
    }
}

static bool file_open (char *filename)
{
    if(file.handle)
        file_close();

    if(f_open(&cncfile, filename, FA_READ) == FR_OK) {
        file.handle = &cncfile;
        file.size = f_size(file.handle);
        file.pos = 0;
        file.line = 0;
        file.eol = false;
    }

    return file.handle != NULL;
}

static int16_t file_read (void)
{
    signed char c;
    UINT count;

    if(f_read(file.handle, &c, 1, &count) == FR_OK && count == 1)
        file.pos = f_tell(file.handle);
    else
        c = -1;

    if(c == '\r' || c == '\n')
        file.eol++;
    else
        file.eol = 0;

    return (int16_t)c;
}

static bool sdcard_mount (void)
{
    if(file.fs == NULL)
        file.fs = malloc(sizeof(FATFS));

    if(file.fs && f_mount(0, file.fs) != FR_OK) {
        free(file.fs);
        file.fs = NULL;
    }

    return file.fs != NULL;
}

static status_code_t sdcard_ls (char *buf)
{
    char path[MAX_PATHLEN] = ""; // NB! also used as work area when recursing directories

    return scan_dir(path, 10, buf) == FR_OK ? Status_OK : Status_SDFailedOpenDir;
}

static void sdcard_end_job (void)
{
    file_close();
    hal.stream_reset_read_buffer();
    hal.stream_read = readfn;
    hal.stream_suspend_read = suspend_readfn;
    hal.driver_rt_report = NULL;
    hal.report_status_message = report_status_message;
    sys.block_input_stream = false;
}

void trap_status_report (status_code_t status_code)
{
    if(status_code != Status_OK) { // TODO: all errors should terminate job?
        char buf[50]; // TODO: check if extended error reports are permissible
        sprintf(buf, "error:%d in SD file at line %lu\r\n", (uint8_t)status_code, file.line);
        hal.stream_write(buf);

        sdcard_end_job();
    }
    // else
    //     file.line++; ??
}

static int16_t sdcard_read (void)
{
    int16_t c = -1;

    if(file.eol == 1)
        file.line++;

    if(file.handle) {

        if(sys.state == STATE_IDLE || (sys.state & (STATE_CYCLE|STATE_HOLD)))
            c = file_read();

        if(c == -1) { // EOF or error reading or grbl problem
            file_close();
            if(file.eol == 0) // Return newline if line was incorrectly terminated
                c = '\n';
        }

    } else if(sys.state == STATE_IDLE) // TODO: end on ok count match line count?
        sdcard_end_job();

    return c;
}

static void sdcard_report (stream_write_ptr stream_write)
{
    stream_write("|SD:");
    stream_write(ftoa((float)file.pos / (float)file.size * 100.0f, 1));
}

static bool sdcard_suspend (bool suspend)
{
    sys.block_input_stream = !suspend;
    if(suspend) {
        hal.stream_reset_read_buffer();
        hal.stream_read = readfn;                           // Restore serial input for tool change (jog etc)
        hal.report_status_message = report_status_message;  // as well as normal status messages reporting
    } else {
        hal.stream_read = sdcard_read;                      // Resume reading from SD card
        hal.report_status_message = trap_status_report;     // and redirect status messages back to us
    }

    return true;
}

static status_code_t sdcard_parse (uint_fast16_t state, char *line, char *lcline)
{
	lcline = lcline;
    status_code_t retval = Status_Unhandled;

    if(line[1] == 'F') switch(line[2]) {

        case '\0':
            retval = sdcard_ls(line); // (re)use line buffer for reporting filenames
            break;

        case 'M':
            retval = sdcard_mount() ? Status_OK : Status_SDMountError;
            break;

        case '=':
            if (state != STATE_IDLE)
                retval = Status_SystemGClock;
            else {
                if(file_open(&line[3])) {
                    gc_state.last_error = Status_OK;                // Start with no errors
                    hal.report_status_message(Status_OK);           // and confirm command to originator
                    readfn = hal.stream_read;                       // Save serial read fn
                    suspend_readfn = hal.stream_suspend_read;       // and suspend fn pointers
                    hal.stream_read = sdcard_read;                  // then redirect to read from SD card instead
                    hal.stream_suspend_read = sdcard_suspend;       // ...
                    hal.driver_rt_report = sdcard_report;      // Sdd percent complete to real time report
                    hal.report_status_message = trap_status_report; // Redirect status message reports here
                    sys.block_input_stream = true;                  // Block serial input other than real time commands TODO: remove?
                    retval = Status_OK;
                } else
                    retval = Status_SDReadError;
            }
            break;

        default:
            retval = Status_InvalidStatement;
            break;
    }

    return retval;
}

void sdcard_reset (void)
{
	if(hal.stream_read == sdcard_read) {
		char buf[70];
		sprintf(buf, "[MSG:Reset during streaming of SD file at line: %lu]\r\n", file.line);
		hal.stream_write_all(buf);
		sdcard_end_job();
	}
}

void sdcard_init (void)
{
    hal.driver_sys_command_execute = sdcard_parse;
}

#endif
