#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include <switch.h>

#define TESLA_INIT_IMPL
#include <tesla.hpp>

class ModifiedStepTrackBar : public tsl::elm::StepTrackBar {
public:

    // La barra recibe 2 cosas, icono y un slider tambien se  
    // encarga de asignar cuantos pasos tendra el slider
    ModifiedStepTrackBar(const char *icon, std::initializer_list<std::string> stepDescriptions)
        : StepTrackBar(icon, stepDescriptions.size()),
          savedStepDescriptions(stepDescriptions.begin(), stepDescriptions.end()),
          cachedWidths(stepDescriptions.size(), -1) { }

    // Se remplaza el metodo de dibujo de un slider normal
    void draw(tsl::gfx::Renderer *renderer) override {
        // Se dibuja primero el slider
        StepTrackBar::draw(renderer);

        // Comprueba si existen textos y obtiene los disponibles
        const auto numSteps = savedStepDescriptions.size();
        // Si no hay ninguna, sale de draw()
        if (numSteps == 0)
            return;
        
        // Define dónde empieza y termina visualmente la barra
        const s32 trackStart = getX() + 60;
        const s32 trackEnd = getX() + getWidth() - 35;
        const s32 trackWidth = trackEnd - trackStart;

        // Hace el dibujado de textos, dependiendo de la cantidad disponible
        // y calcula la posición de cada texto
        for (size_t i = 0; i < numSteps; ++i) {
            const float fraction = (numSteps == 1)
                ? 0.0f
                : static_cast<float>(i) / static_cast<float>(numSteps - 1);
                
            const s32 stepCenterX = trackStart + static_cast<s32>(trackWidth * fraction);
            const char* text = savedStepDescriptions[i].c_str();

            // Calculá el ancho solo en el primer frame y reutilizarlo
            if (cachedWidths[i] < 0) {
                auto [descWidth, descHeight] = renderer->drawString(
                    text, false, 0, 0, 15,
                    tsl::style::color::ColorTransparent
                );
                cachedWidths[i] = descWidth;
            }

            // Centra el texto exactamente donde esta el punto del slider
            renderer->drawString(
                text, false,
                stepCenterX - (cachedWidths[i] / 2), getY() + 20, 15,
                tsl::style::color::ColorDescription
            );
        }
    }

// Variable exclusiva de cada objeto de ModifiedStepTrackBar,
// que almacena las descripciones de los pasos
private:
    std::vector<std::string> savedStepDescriptions;
    std::vector<s32> cachedWidths;
};

class NXAC_GUI : public tsl::Gui {
public:
    constexpr static float MasterVolumeDefault = 1.0f;
    constexpr static std::array<float, 5> MasterVolumeSteps = {
        1.0f, 1.5f, 2.0f, 2.5f, 3.0f
    };
    constexpr static auto ConfigDirPath = "/config/NX-AudioControl";
    constexpr static auto ConfigFilePath = "/config/NX-AudioControl/config.bin";

NXAC_GUI() {
    bool hasVolume = false;
    tsl::hlp::doWithSDCardHandle([this, &hasVolume] {
        auto *fs = fsdevGetDeviceFileSystem("sdmc");
        if (!fs)
            return;

        FsFile fp;
        if (R_FAILED(fsFsOpenFile(fs, ConfigFilePath, FsOpenMode_Read, &fp)))
            return;
        tsl::hlp::ScopeGuard guard([&fp] { fsFileClose(&fp); });

        float volume;
        u64 read;
        if (R_FAILED(fsFileRead(&fp, 0, &volume, sizeof(volume), FsReadOption_None, &read)) ||
            read != sizeof(volume))
            return;

        master_volume = volume;
        hasVolume = true;
    });

    if (!hasVolume)
        audctlGetSystemOutputMasterVolume(&master_volume);

    // Convierte cualquier valor anterior al paso válido más cercano.
    master_volume = stepToVolume(volumeToStep(master_volume));
    audctlSetSystemOutputMasterVolume(master_volume);

    // Se determina el estado inicial.
    volume_slider_locked = isCurrentTargetBlocked();
    if (volume_slider_locked) {
        // Se guarda el master volume del usuario.
        volume_before_lock = master_volume;
        // En un destino bloqueado se fuerza el volumen al valor default (x1.0).
        master_volume = MasterVolumeDefault;
        audctlSetSystemOutputMasterVolume(master_volume);
    }
}

    ~NXAC_GUI
() override {
        tsl::hlp::doWithSDCardHandle([this] {
            auto *fs = fsdevGetDeviceFileSystem("sdmc");
            if (!fs)
                return;
            if (auto rc = fsFsCreateDirectory(fs, ConfigDirPath);
                R_FAILED(rc) && R_DESCRIPTION(rc) != 2)
                return;
            if (auto rc = fsFsCreateFile(fs, ConfigFilePath, sizeof(master_volume), 0);
                R_FAILED(rc) && R_DESCRIPTION(rc) != 2)
                return;

            FsFile fp;
            if (R_FAILED(fsFsOpenFile(fs, ConfigFilePath, FsOpenMode_Write, &fp)))
                return;
            tsl::hlp::ScopeGuard guard([&fp] { fsFileClose(&fp); });

            // Cuando master_volume se bloquea su valor es x1.0 por seguridad.
            // Pero tambien se guarda el master_volume que ya tenia el usuario.
            const float volumeToPersist = volume_slider_locked
                ? volume_before_lock
                : master_volume;
            fsFileWrite(&fp, 0, &volumeToPersist, sizeof(volumeToPersist), FsWriteOption_None);
        });
    }

    // Dibuja el nombre del overlay y debajo en gris claro la version y el author del overlay.
    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame(
            APP_TITLE,
            std::string("v") + APP_VERSION + "    Made by " + APP_AUTHOR
        );

        root_frame = frame;
        rebuildContent();
        return frame;
    }

    void update() override {
        const bool wasLocked = volume_slider_locked;

        refreshAudioSafetyState();

        if (volume_slider_locked != wasLocked) {
            this->removeFocus();
            rebuildContent();
            this->requestFocus(root_frame, tsl::FocusDirection::None);
            return;
        }

        if (volume_slider_locked) {
            mvol_header->setText("Master Volume:\n");
        } else {
            mvol_header->setText(
                "Master Volume:                                           \ue0e2 Reset\n"
            );
        }
    }

    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                     HidAnalogStickState leftJoyStick,
                     HidAnalogStickState rightJoyStick) override {
        if (!volume_slider_locked && mvol_slider && (keysDown & HidNpadButton_X)) {
            master_volume = MasterVolumeDefault;
            mvol_slider->setProgress(volumeToStep(master_volume));
            audctlSetSystemOutputMasterVolume(master_volume);
            return true;
        }

        return tsl::Gui::handleInput(
            keysDown, keysHeld, touchPos, leftJoyStick, rightJoyStick
        );
    }

private:
    static constexpr float stepToVolume(std::uint8_t step) {
        return MasterVolumeSteps[step < MasterVolumeSteps.size() ? step : 0];
    }

    static std::uint8_t volumeToStep(float volume) {
        std::uint8_t nearest = 0;
        float nearestDistance = std::abs(volume - MasterVolumeSteps[0]);

        for (std::uint8_t i = 1; i < MasterVolumeSteps.size(); ++i) {
            const float distance = std::abs(volume - MasterVolumeSteps[i]);
            if (distance < nearestDistance) {
                nearest = i;
                nearestDistance = distance;
            }
        }

        return nearest;
    }

// Targets bloqueados por seguridad, si el AudioTarget actual es uno de estos
// // el volumen se establece automaticamente en su valor por defecto (x1.0).
static bool isBlockedTarget(AudioTarget target) {
    return target == AudioTarget_Headphone ||
           target == AudioTarget_Tv ||
           target == AudioTarget_UsbOutputDevice ||
           target == AudioTarget_Bluetooth;
}
        
// Comprueba si el destino de audio actual está bloqueado.
// Si es un target bloqueado, se bloquea por seguridad.
bool isCurrentTargetBlocked() const {
    AudioTarget activeTarget;

    if (R_FAILED(audctlGetActiveOutputTarget(&activeTarget)))
        return true;

    return isBlockedTarget(activeTarget);
}

// Actualiza el estado de seguridad del volumen.
void refreshAudioSafetyState() {
    const bool shouldLock = isCurrentTargetBlocked();

    // Se hace un cambio de estado si se registro un
    // cambio entre bloqueado y desbloqueado
    if (shouldLock != volume_slider_locked) {
        volume_slider_locked = shouldLock;

        // Antes de bloquear y establecer el master volume en su
        // valor default (x1.0) guardamos el estado actual.
        if (volume_slider_locked) {
            volume_before_lock = master_volume;

            // Se fuerza el master volume al valor x1.0
            master_volume = MasterVolumeDefault;
            audctlSetSystemOutputMasterVolume(master_volume);
        } else {
            // Antes de desbloquear, se restaura el master volume del usuario
            master_volume = volume_before_lock;
            audctlSetSystemOutputMasterVolume(master_volume);
        }
    }
}
    
    // Reconstruye el contenido del frame según el estado de bloqueo actual.
    void rebuildContent() {
        auto *list = new tsl::elm::List();

        mvol_header = new tsl::elm::CategoryHeader("");

        if (volume_slider_locked) {
            mvol_slider = nullptr;

            // Header de la sección.
            mvol_header->setText("Master Volume:\n");
            list->addItem(mvol_header);

            // Mensaje informativo.
            auto *lockedText = new tsl::elm::CustomDrawer(
                [](tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 w, s32 h) {
                const auto color = renderer->a(0xF33F);
                renderer->drawString(
                    "This  menu  is disabled  for hearing safety reasons.",
                     false, x + 13, y + 11, 14, color
                    );
                renderer->drawString(
                    "Master volume cannot be adjusted with this audio",
                     false, x + 13, y + 26, 14, color
                    );
                renderer->drawString(
                    "output. You  can  only  adjust  the  master volume",
                     false, x + 13, y + 41, 14, color
                    );
                renderer->drawString(
                    "when using the  built-in Nintendo Switch speakers.",
                     false, x + 13, y + 56, 14, color
                    );
                }
            );

            lockedText->setBoundaries(0, 0, tsl::cfg::FramebufferWidth, 70);
            list->addItem(lockedText);
        } else {
            mvol_slider = new ModifiedStepTrackBar("", {
                "100%", "150%", "200%", "250%", "300%"
            });

            mvol_slider->setProgress(volumeToStep(master_volume));

            mvol_slider->setValueChangedListener([this](std::uint8_t step) {
                if (volume_slider_locked) {
                    mvol_slider->setProgress(volumeToStep(master_volume));
                    return;
                }

                master_volume = stepToVolume(step);
                audctlSetSystemOutputMasterVolume(master_volume);
            });

            mvol_header->setText(
                "Master Volume:                                           \ue0e2 Reset\n"
            );
            list->addItem(mvol_header);
            list->addItem(mvol_slider);
        }

    root_frame->setContent(list);
}

private:
    tsl::elm::OverlayFrame *root_frame = nullptr;
    tsl::elm::CategoryHeader *mvol_header = nullptr;
    ModifiedStepTrackBar *mvol_slider = nullptr;

    float master_volume = MasterVolumeDefault;
    float volume_before_lock = MasterVolumeDefault;
    bool volume_slider_locked = false;
};

class MasterVolumeOverlay : public tsl::Overlay {
public:
    void initServices() override {
        audctlInitialize();
    }

    void exitServices() override {
        audctlExit();
    }

    void onShow() override { }
    void onHide() override { }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<NXAC_GUI
    >();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<MasterVolumeOverlay>(argc, argv);
}