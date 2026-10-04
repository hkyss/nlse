#include "nlse.h"

namespace {

const NLSEInterface* nlse;
NLSEPluginHandle self;

void receive(const NLSEMessage* message)
{
    if (message->type == NLSE_MESSAGE_POST_LOAD) {
        nlse->Log(self, "every plugin is loaded");
    }
}

}

NLSE_EXPORT const NLSEPluginVersion NLSEPlugin_Version = {NLSE_API_VERSION, "Example", "1.0", "hkyss"};

NLSE_EXPORT bool NLSEPlugin_Load(const NLSEInterface* api)
{
    nlse = api;
    self = nlse->GetPluginHandle();
    const uint64_t game = nlse->gameVersion;
    nlse->Log(self, "loaded into Norland %llu.%llu.%llu.%llu", game >> 48, (game >> 32) & 0xFFFF, (game >> 16) & 0xFFFF,
              game & 0xFFFF);
    return nlse->RegisterListener(self, "NLSE", receive);
}
