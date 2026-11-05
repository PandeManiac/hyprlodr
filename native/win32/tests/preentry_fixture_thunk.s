        .text
        .globl startup_fixture_callsite
        .def startup_fixture_callsite; .scl 2; .type 32; .endef
startup_fixture_callsite:
        subq    $40, %rsp
        callq   startup_fixture_original
        addq    $40, %rsp
        retq
