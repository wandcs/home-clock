#include "alarm_trigger_audio.h"
bool at_dispatch_audio(at_state *s,aa_player *player) {
    if(!s||!player)return false;
    at_audio_command command;
    if(!at_take_audio(s,&command))return false;
    aa_result result=AA_OK;
    if(command.kind==AT_AUDIO_START)result=aa_start(player);
    else aa_stop(player);
    return at_accept_audio(s,command,result);
}
