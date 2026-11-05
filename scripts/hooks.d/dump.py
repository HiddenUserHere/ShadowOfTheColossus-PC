"""Resource dump (gpu/sotc/probe_dump.cpp, SOTC_RES_DUMP=<dir>). Observe only.

RES_CREATE  site 0x147182 in ResourceFactory::CreateResource 0x147160 (the loader; rsi = its request, param_2);
            the entry is a tail-call jump target, so the entry detour is refused. Keeps the
            request for the RES_LOADED handler of the same thread (the name is at request+0xa8).
            Found with tools/re/eboot_re.py sig 0x147160 -n 28.
CWKS_BODY   entry of 0x14edc0, ResourcePool::RecordResourceDependencies (rdi = the pool). Dumps the workspace
            dependency table (the .cwks body, pool+0x130) once, before the job frees it. The file is read through
            0x1512b0, not as a resource, so RES_LOADED never sees it.
            Found with tools/re/eboot_re.py sig 0x14edc0 -n 40.

The RES_LOADED hook (entry of 0x4d470, scripts/hooks.d/csdr.py) also feeds the dump, but its own switch is
SOTC_RES_LOADED: set SOTC_RES_LOADED=1 with SOTC_RES_DUMP (sotc.py sets it only for its probe switches).
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='RES_CREATE',
        switch='SOTC_RES_DUMP',
        group='res_dump_create',
        # Not the entry: the function tail-calls itself with a jmp to 0x147160, which the entry detour refuses.
        # Site after the prologue, where rsi is still the request: mov r15,rsi; mov rax,[r13];
        # mov [rbp-0x30],rax; mov rbx,[r15]; mov rdi,rbx; call 0x14d210
        mode='site',
        pattern='49 89 f7 49 8b 45 00 48 89 45 d0 49 8b 1f 48 89 df e8',
        expected={EU_FINGERPRINT: 0x147182},
    ),
    dict(
        name='CWKS_BODY',
        switch='SOTC_RES_DUMP',
        group='res_dump_body',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x68; mov rax,[rip+..]; mov rbx,rdi; mov rax,[rax];
        # mov [rbp-0x30],rax; mov r13,[rbx+0x130]
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 48 8b 05 ?? ?? ?? ?? 48 89 fb 48 8b 00 48 89 45 d0 '
                '4c 8b ab 30 01 00',
        expected={EU_FINGERPRINT: 0x14edc0},
    ),
]
