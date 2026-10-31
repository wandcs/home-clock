#ifndef CLOCK_ALARM_TRIGGER_AUDIO_H
#define CLOCK_ALARM_TRIGGER_AUDIO_H
#include "alarm_trigger.h"
/* Single owner calls immediately after step/readback; invokes only the existing
 * nonblocking request APIs, never aa_pump/codec. Status is supplied by the caller
 * to at_step via aa_status. No request acknowledgement claims physical sound. */
bool at_dispatch_audio(at_state *,aa_player *);
#endif
