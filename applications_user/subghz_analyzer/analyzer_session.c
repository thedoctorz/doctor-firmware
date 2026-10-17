#include "analyzer_session.h"

#include <string.h>

#define ANALYZER_SEED_BW_HEADROOM_NUM 3u
#define ANALYZER_SEED_BW_HEADROOM_DEN 2u

#define ANALYZER_SEED_MIN_BINS 3u

const char* analyzer_pipeline_name(AnalyzerPipelineStage stage) {
    switch(stage) {
    case AnalyzerPipelineHunt:
        return "Frequency";
    case AnalyzerPipelineSet:
        return "Set";
    case AnalyzerPipelineLock:
        return "Spectrum";
    case AnalyzerPipelineTune:
        return "Modulation";
    case AnalyzerPipelineResult:
    default:
        return "Save";
    }
}

void analyzer_session_init(AnalyzerSession* session) {
    memset(session, 0, sizeof(*session));
    session->frequency = 433920000;
    session->floor_raw = ANALYZER_ENV_FLOOR_INVALID;
    session->seed_mod = AnalyzerModCount;
}

void analyzer_session_clear_lock(AnalyzerSession* session) {
    session->lock_valid = false;
    session->env_vote = AnalyzerEnvVoteNone;
    session->env_confidence = 0;
    session->atten = 0;
    session->floor_raw = ANALYZER_ENV_FLOOR_INVALID;
    session->floor_dbm = 0.0f;
    session->peak_dbm = 0.0f;
    session->survey_passes = 0;
    memset(&session->survey, 0, sizeof(session->survey));

    session->seed_mod = AnalyzerModCount;
    session->seed_bandwidth_hz = 0;
    session->seed_deviation_hz = 0;
    session->seed_rate_baud = 0;
}

void analyzer_session_derive_seed(AnalyzerSession* session) {
    session->seed_mod = AnalyzerModCount;
    session->seed_bandwidth_hz = 0;
    session->seed_deviation_hz = 0;
    session->seed_rate_baud = 0;

    if(session->env_vote == AnalyzerEnvVoteAm) {
        session->seed_mod = AnalyzerModOok;
    } else if(session->env_vote == AnalyzerEnvVoteFm) {
        session->seed_mod = AnalyzerMod2Fsk;
    }

    const AnalyzerSurvey* survey = &session->survey;
    if(!survey->valid) return;
    if(survey->bins_over_floor < ANALYZER_SEED_MIN_BINS) return;

    if(survey->occupied_bw_hz) {
        session->seed_bandwidth_hz = (survey->occupied_bw_hz * ANALYZER_SEED_BW_HEADROOM_NUM) /
                                     ANALYZER_SEED_BW_HEADROOM_DEN;
    }

    if(survey->twin_lobe && survey->lobe_split_hz) {
        session->seed_deviation_hz = survey->lobe_split_hz / 2u;
    }
}
