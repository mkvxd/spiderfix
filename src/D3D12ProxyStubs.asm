OPTION CASEMAP:NONE

; Repasse por jump-table preenchida no InitOriginalD3D12 (g_procs).
; Caminho rapido vira so mov/test/jz/jmp antes do prologo: preserva tudo (so
; R10, scratch nao-argumento, e tocado) e o unwinder ve a pilha intacta.
; Entrada nula (jogo chamou export antes de D3D12CreateDevice, ex. via
; DeviceFactory): slow path com FRAME real no proprio stub, salva
; RCX/RDX/R8/R9 + XMM0-3 FORA do home space do callee ([rsp+00h..1Fh]),
; chama SpiderFixEnsureProcs (garante g_procs sem entradas nulas, com
; fallback E_NOTIMPL/0), restaura tudo e sai por jmp RIP-relativo, que o
; unwinder x64 reconhece como epilogo.
; Ordem dos indices sincronizada com kNames em SpiderD3D12Proxy.cpp.

EXTERN g_procs:QWORD
EXTERN SpiderFixEnsureProcs:PROC

.code

; Frame 88h: entrada rsp = 8 mod 16; 8 + 88h = 90h, rsp 16-alinhado p/ movaps
; e call. [rsp+00h..1Fh] = home space do callee, nao usado aqui.
JMP_SLOT MACRO procName, idx
procName PROC FRAME
    mov r10, qword ptr [g_procs + idx*8]
    test r10, r10
    jz @F
    jmp r10
@@:
    sub rsp, 88h
    .allocstack 88h
    .endprolog
    movaps [rsp+20h], xmm0
    movaps [rsp+30h], xmm1
    movaps [rsp+40h], xmm2
    movaps [rsp+50h], xmm3
    mov [rsp+60h], rcx
    mov [rsp+68h], rdx
    mov [rsp+70h], r8
    mov [rsp+78h], r9
    call SpiderFixEnsureProcs
    movaps xmm0, [rsp+20h]
    movaps xmm1, [rsp+30h]
    movaps xmm2, [rsp+40h]
    movaps xmm3, [rsp+50h]
    mov rcx, [rsp+60h]
    mov rdx, [rsp+68h]
    mov r8, [rsp+70h]
    mov r9, [rsp+78h]
    add rsp, 88h
    jmp qword ptr [g_procs + idx*8]
procName ENDP
ENDM

JMP_SLOT SetAppCompatStringPointer, 0
JMP_SLOT D3D12GetDebugInterface, 1
JMP_SLOT D3D12CoreCreateLayeredDevice, 2
JMP_SLOT D3D12CoreGetLayeredDeviceSize, 3
JMP_SLOT D3D12CoreRegisterLayers, 4
JMP_SLOT D3D12CreateRootSignatureDeserializer, 5
JMP_SLOT D3D12CreateVersionedRootSignatureDeserializer, 6
JMP_SLOT D3D12EnableExperimentalFeatures, 7
JMP_SLOT D3D12GetInterface, 8
JMP_SLOT D3D12PIXEventsReplaceBlock, 9
JMP_SLOT D3D12PIXGetThreadInfo, 10
JMP_SLOT D3D12PIXNotifyWakeFromFenceSignal, 11
JMP_SLOT D3D12PIXReportCounter, 12
JMP_SLOT D3D12SerializeRootSignature, 13
JMP_SLOT D3D12SerializeVersionedRootSignature, 14
JMP_SLOT GetBehaviorValue, 15
JMP_SLOT D3D12Ordinal99, 16

END
