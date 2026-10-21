// dc2tk.h: lets a C compiler or IDE read the scripts. dc2tk's compiler does not read this file:
// the macros below are its built-ins, and here they give C tools a standard C99 meaning for them.
#ifndef DC2TK_H
#define DC2TK_H

// Set for C tools only: a header gives them, in #ifdef DC2TK_C_TOOLS, what C cannot say as dc2tk
// does (a helper taking a parameter of type any).
#define DC2TK_C_TOOLS

typedef const char *string;
// A value of whatever type a call passes. Only a native's parameters and value kinds (message,
// map_id) have it, and VM_ARGS(...) hides those parameters from C tools; here it is a name.
typedef int any;

// A native's declaration: extern int get_flag(int flag) NATIVE(14);. OPTIONAL(...) holds the
// parameters a call may leave out; VM_ARGS(...) holds a parameter list C cannot declare (one of
// type any, which takes whatever the call passes, or OPTIONAL(...) first), which C tools read as
// a function of unchecked parameters.
#define NATIVE(...)
#define OPTIONAL(...) ...
#define VM_ARGS(...) ()
#define PROGRAM(number, function) extern int dc2tk_program_##function
#define MAIN(function) extern int dc2tk_main_##function
#define AT(base)
#define DIRECT
// The folder of the mod's own new files on the disc ("mods/<id>/"), for paths such as
// MOD_DIR "calc.chr"; build defines it for each mod's patches and map scripts.
#ifndef MOD_DIR
#define MOD_DIR "mods/mod.id/"
#endif

// The VM's statements and instructions.
void dc2tk_wait(void);
void dc2tk_end(void);
void dc2tk_skip_end(void);
void dc2tk_print();
float dc2tk_sin(float angle);
float dc2tk_cos(float angle);
#define WAIT() dc2tk_wait()
#define END() dc2tk_end()
#define SKIP_END() dc2tk_skip_end()
#define PRINT(...) dc2tk_print(__VA_ARGS__)
#define SIN(angle) dc2tk_sin(angle)
#define COS(angle) dc2tk_cos(angle)

// Where the VM's values differ from C's (docs/scripting/language.md). The VM keeps a value's own
// type where it is stored, so FLOAT_AS_INT(x) is x stored, passed or returned as an int with no
// conversion (the int variable then holds a float, or a native's float result's bits), and
// INT_AS_FLOAT(x) the same for a float. AND_VALUE(a, b) and OR_VALUE(a, b) are a && b and a || b
// as the VM gives them: 0 or 1 when a decides, else b's own value, where C gives b as 0 or 1.
// DOUBLE_STORE(x = value) stores value in x twice, as x = x = value would, which C leaves
// undefined.
#define FLOAT_AS_INT(value) ((int)(value))
#define INT_AS_FLOAT(value) ((float)(value))
#define AND_VALUE(a, b) ((a) ? (b) : 0)
#define OR_VALUE(a, b) ((a) ? 1 : (b))
#define DOUBLE_STORE(assignment) (assignment)

// Mod patches (docs/modding/mods.md). NEW_FLAG(name) and its kin are numbers build gives a mod;
// here they read as constants. original(...) in a #pragma override function calls the function it
// replaced. LANGUAGE is the language of the script a patch is merged into; here it reads as English.
#define NEW_FLAG(name) enum { name = 1024 }
#define NEW_COUNTER(name) enum { name = 32 }
#define NEW_PROGRAM(name) enum { name = 9000 }
#define NEW_MONSTER_VAR(name) enum { name = 8 }
int dc2tk_original();
#define original dc2tk_original
#define LANGUAGE_JAPANESE 0
#define LANGUAGE_ENGLISH 1
#define LANGUAGE_FRENCH 2
#define LANGUAGE_GERMAN 3
#define LANGUAGE_ITALIAN 4
#define LANGUAGE_SPANISH 5
#ifndef LANGUAGE
#define LANGUAGE LANGUAGE_ENGLISH
#endif

#endif
