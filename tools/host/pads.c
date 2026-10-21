/* make pads: list the game controllers SDL sees (the same ones the game window uses). */
#include <SDL.h>
#include <stdio.h>

int main(void) {
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) { printf("SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    SDL_Delay(1500);   /* Bluetooth pads can take a moment to show up */
    SDL_GameControllerUpdate();
    int n = SDL_NumJoysticks();
    printf("%d controller(s) found\n", n);
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) { printf("  #%d %s (joystick without a known mapping)\n", i, SDL_JoystickNameForIndex(i)); continue; }
        SDL_GameController *c = SDL_GameControllerOpen(i);
        printf("  #%d %s, type %d\n", i, SDL_GameControllerName(c), (int)SDL_GameControllerGetType(c));
        SDL_GameControllerClose(c);
    }
    SDL_Quit();
    return 0;
}
