/* The standalone fuzz harness entry point.
 *
 * fuzz_roundtrip.c holds the cases and both harness entry points; this file
 * supplies only main, and exists so that fuzz_roundtrip.c can be compiled on
 * its own and linked against the libFuzzer runtime, which defines main
 * itself. Splitting the files is what keeps the two shapes from colliding;
 * a macro that each build had to define would have to be right in two
 * places, and a build that forgot it produced a duplicate main.
 *
 * Built into zgec_fuzz by CMakeLists.txt; libFuzzer's own entry point does
 * not compile this file at all.
 */

/* Declared in fuzz_roundtrip.c; repeated so the definition is checked
 * against the same signature and -Wmissing-prototypes stays quiet. */
int zgec_fuzz_standalone_main(int argc, char **argv);

int main(int argc, char **argv)
{
    return zgec_fuzz_standalone_main(argc, argv);
}
