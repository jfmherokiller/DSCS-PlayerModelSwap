// Shared between the plugin core (PlayerModelSwap.cpp) and the in-game menu (Overlay.cpp).
#pragma once
#include <string>
#include <vector>

namespace pms
{
    struct ModelEntry
    {
        std::string code; // "chr090"
        std::string name; // localized Digimon name, may be empty
        bool fieldMotions; // ships its own field idle (fn01)
    };

    void log(const std::string& message);

    // Game thread only (called from the SwapBuffers hook).
    std::vector<ModelEntry> listModels();
    void onFrame();

    std::string getModel();
    void setModel(const std::string& code); // "" = protagonist; persisted to the INI, applied at once in the field

    bool getEnabled();
    void setEnabled(bool enabled);
    bool getTemporaryHuman();
    void setTemporaryHuman(bool human);
    bool getReplaceInCutscenes();
    void setReplaceInCutscenes(bool replace);
} // namespace pms

namespace overlay
{
    // Hooks SwapBuffers and the game's input reads; menuKey 0 disables the menu.
    void install(int menuKey);
}
