#include "App.h"

namespace {
App app;
}

void setup() {
	app.init();
}

void loop() {
	app.tick();
}