package com.plasticcog.degrees720;

import org.libsdl.app.SDLActivity;

/**
 * 720 Degrees: SDL's activity loads libSDL3.so and libmain.so and runs the game's main()
 * (port/src/app/main.cpp) on its own thread.
 */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
