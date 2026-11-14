#ifndef LANNATIVE_CONSOLE_H
#define LANNATIVE_CONSOLE_H

// The constructor may run only after all three engine objects exist, and never when a console is already attached.
static int console_should_construct(void *engine, void *viewport, void *console_class, void *viewport_console) {
    return engine && viewport && console_class && !viewport_console;
}

#endif
