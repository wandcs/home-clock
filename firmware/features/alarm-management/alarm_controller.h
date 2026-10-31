#ifndef ALARM_CONTROLLER_H
#define ALARM_CONTROLLER_H
#include "alarm_config.h"
typedef enum { AM_IO_OK, AM_IO_MISSING, AM_IO_BAD, AM_IO_FOREIGN, AM_IO_ERROR,
               AM_IO_SAVED, AM_IO_DIFFERENT, AM_IO_UNCERTAIN } am_io_status;
typedef struct { am_io_status status; am_config config; int write_error,read_error; } am_io_result;
typedef enum { AM_JOB_LOAD, AM_JOB_SAVE } am_job_kind;
typedef struct { uint32_t token; am_job_kind kind; am_config expected,proposed; } am_job;
typedef struct { uint32_t token; am_io_result result; } am_reply;
typedef enum { AM_LIST, AM_EDIT, AM_REPEAT, AM_DELETE } am_view;
typedef enum { AM_NOTE_LOADING, AM_NOTE_NONE, AM_NOTE_SAVING, AM_NOTE_FULL,
               AM_NOTE_INVALID, AM_NOTE_FAILED, AM_NOTE_CHANGED, AM_NOTE_UNCERTAIN,
               AM_NOTE_BAD, AM_NOTE_FOREIGN, AM_NOTE_READ_ERROR } am_note;
typedef struct {
    am_config confirmed;
    am_entry draft;
    am_view view;
    am_note note;
    bool known,blocked,busy,draft_new,recovering;
    uint32_t token,revision,draft_epoch;
    am_job job;
} am_controller;
typedef enum { AM_SUMMARY_HIDDEN, AM_SUMMARY_WAIT, AM_SUMMARY_UNKNOWN, AM_SUMMARY_NEXT } am_summary_status;
typedef struct { am_summary_status status; af_time at; bool fallback; uint32_t id; } am_summary;
void am_init(am_controller *c);
bool am_reload(am_controller *c);
bool am_begin(am_controller *c,uint32_t id); /* id=0: new draft */
bool am_edit_time(am_controller *c,int hour,int minute);
bool am_edit_mode(am_controller *c,unsigned mode); /* Inline AM_EDIT; AM_REPEAT retained for old callers. */
bool am_edit_enabled(am_controller *c,bool enabled);
bool am_show_repeat(am_controller *c);
bool am_back(am_controller *c);
bool am_ask_delete(am_controller *c);
bool am_save(am_controller *c);
bool am_delete(am_controller *c);
bool am_toggle(am_controller *c,uint32_t id,bool enabled);
void am_dispatch_failed(am_controller *c);
void am_request_timeout(am_controller *c); /* No assertion that media stayed unchanged. */
bool am_accept(am_controller *c,const am_reply *reply);
am_summary am_next_summary(const am_controller *c,const af_time *reference,bool trusted,
                           bool include_reference,const af_year *years,size_t year_count);
bool am_calendar_notice(const am_controller *c,const af_time *reference,bool trusted,
                        const af_year *years,size_t year_count);
const char *am_mode_text(unsigned mode);
const char *am_note_text(am_note note);
#endif
