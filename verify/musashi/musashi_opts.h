/* Force-included when building Musashi, so its own sources stay untouched. */
void harness_instr_hook(unsigned int pc);
#define M68K_INSTRUCTION_HOOK 2                 /* M68K_OPT_SPECIFY_HANDLER */
#define M68K_INSTRUCTION_CALLBACK(pc) harness_instr_hook(pc)
