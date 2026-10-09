OPTION CASEMAP:NONE

; Repasse por jump-table preenchida no InitOriginalD3D12 (g_procs).
; Caminho rapido vira so mov/test/jmp: preserva tudo (so R10/R11, scratch
; nao-argumento, sao tocados), nao precisa de unwind (sem call/prologo).
; Entrada nula (jogo chamou export antes de D3D12CreateDevice, ex. via
; DeviceFactory): idx vai em R11 (scratch, nunca argumento) e tail-jmp para
; SpiderFixSlowResolve, stub continua folha. Slow path tem FRAME real, salva
; RCX/RDX/R8/R9 + XMM0-3 (todos volateis que a chamada C++ suja), resolve,
; restaura tudo e jmp. Entradas ausentes apontam para fallback local
; (E_NOTIMPL p/ HRESULT, 0 p/ ponteiro/void/UINT64).
; Ordem dos indices sincronizada com kNames em SpiderD3D12Proxy.cpp.

EXTERN g_procs:QWORD
EXTERN SpiderFixResolveProc:PROC

.code

JMP_SLOT MACRO procName, idx
procName PROC
    mov r10, qword ptr [g_procs + idx*8]
    test r10, r10
    jnz @F
    mov r11d, idx
    jmp SpiderFixSlowResolve
@@: jmp r10
procName ENDP
ENDM

; idx em r11d na entrada. Frame 68h mantem rsp 16-alinhado p/ movaps e call.
SpiderFixSlowResolve PROC FRAME
    sub rsp, 68h
    .allocstack 68h
    .endprolog
    mov [rsp+40h], rcx
    mov [rsp+48h], rdx
    mov [rsp+50h], r8
    mov [rsp+58h], r9
    movaps [rsp+00h], xmm0
    movaps [rsp+10h], xmm1
    movaps [rsp+20h], xmm2
    movaps [rsp+30h], xmm3
    mov [rsp+60h], r11
    mov ecx, r11d
    call SpiderFixResolveProc
    mov rcx, [rsp+40h]
    mov rdx, [rsp+48h]
    mov r8, [rsp+50h]
    mov r9, [rsp+58h]
    movaps xmm0, [rsp+00h]
    movaps xmm1, [rsp+10h]
    movaps xmm2, [rsp+20h]
    movaps xmm3, [rsp+30h]
    mov r10, rax
    add rsp, 68h
    jmp r10
SpiderFixSlowResolve ENDP

JMP_SLOT SetAppCompatStringPointer, 0
JMP_SLOT D3D12GetDebugInterface, 1
JMP_SLOT D3D12CoreCreateLayeredDevice, 2
JMP_SLOT D3D12CoreGetLayeredDeviceSize, 3
JMP_SLOT D3D12CoreRegisterLayers, 4
JMP_SLOT D3D12CreateRootSignatureDeserializer, 5
JMP_SLOT D3D12CreateVersionedRootSignatureDeserializer, 6
JMP_SLOT D3D12DeviceRemovedExtendedData, 7
JMP_SLOT D3D12EnableExperimentalFeatures, 8
JMP_SLOT D3D12GetInterface, 9
JMP_SLOT D3D12PIXEventsReplaceBlock, 10
JMP_SLOT D3D12PIXGetThreadInfo, 11
JMP_SLOT D3D12PIXNotifyWakeFromFenceSignal, 12
JMP_SLOT D3D12PIXReportCounter, 13
JMP_SLOT D3D12SerializeRootSignature, 14
JMP_SLOT D3D12SerializeVersionedRootSignature, 15
JMP_SLOT GetBehaviorValue, 16
JMP_SLOT D3D12Ordinal99, 17

END
