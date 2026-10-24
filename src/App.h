#pragma once

#include "BoardConfig.h"
#include "Types.h"
#include "drivers/LedDriver.h"
#include "drivers/OledDriver.h"
#include "host/Commands.h"
#include "security/Security.h"
#include "storage/ConfigStore.h"
#include "storage/VaultStore.h"
#include "host/SerialLink.h"

class App {
public:
	App();
	App(const App&) = delete;
	App& operator=(const App&) = delete;
	void init();
	void tick();
	const UiSnapshot& snapshot() const;

private:
	static void handleCommand(void* context, const char* command,
		JsonObjectConst arguments, JsonObject response, uint64_t nowMs);
	static void onUiEvent(void* context, UiEvent event, uint64_t nowMs);
	void updateUi(uint64_t nowMs);
	static void onLock(void* context);
	static bool onLoad(void* context);
	static void onMeshEvent(void* context, const MeshEngine::Event& event);
	bool finishReset();

	SerialLink serialLink_;
	RadioDriver radioDriver_;
	ConfigStore configStore_;
	VaultStore vaultStore_;
	Security security_{vaultStore_};
	Contacts contacts_{vaultStore_};
	Groups groups_{vaultStore_};
	Pairing pairing_{security_, contacts_};
	MeshEngine mesh_{contacts_, groups_};
	uint32_t reportedDrops_ = 0;
	Commands::Action action_ = Commands::Action::None;
	uint64_t actionAt_ = 0;
	Commands commands_;
	LedDriver ledDriver_;
	OledDriver oledDriver_;
	UiSnapshot snapshot_;
	LedConfig ledConfig_;
	bool ledReady_ = false;
	bool oledReady_ = false;
#if OFFGRID_SERIAL_NATIVE_USB
	bool wasConnected_ = false;
#endif
};