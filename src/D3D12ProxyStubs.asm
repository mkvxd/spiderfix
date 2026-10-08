OPTION CASEMAP:NONE

; Repasse por jump-table preenchida no InitOriginalD3D12 (g_procs).
; Caminho rapido vira so mov/test/jmp: preserva XMM0-3 e nao precisa de unwind
; (sem call/prologo: unwinder trata como folha, como thunk padrao de proxy).
; Se a entrada ainda for nula (jogo chamou export antes de D3D12CreateDevice,
; ex. via DeviceFactory), tail-jmp para SpiderFixSlowResolve com idx em ecx:
; stub continua folha. Slow path tem FRAME real, salva XMM0-3 e resolve via C++.
; Entradas ausentes apontam para fallback local (E_NOTIMPL p/ HRESULT,
; 0 p/ ponteiro/void/UINT64).
; Ordem dos indices sincronizada com kNames em SpiderD3D12Proxy.cpp.

EXTERN g_procs:QWORD
EXTERN SpiderFixResolveProc:PROC

.code

JMP_SLOT MACRO procName, idx
procName PROC
    mov r10, qword ptr [g_procs + idx*8]
    test r10, r10
    jnz @F
    mov ecx, idx
    jmp SpiderFixSlowResolve
@@: jmp r10
procName ENDP
ENDM

; idx em ecx. Salva XMM0-3 (volateis que a chamada C++ pode sujar), resolve,
; restaura e jmp. Frame 0C8h mantem rsp 16-alinhado p/ movaps e call.
SpiderFixSlowResolve PROC FRAME
    sub rsp, 0C8h
    .allocstack 0C8h
    .endprolog
    movaps [rsp+88h], xmm0
    movaps [rsp+98h], xmm1
    movaps [rsp+0A8h], xmm2
    movaps [rsp+0B8h], xmm3
    mov [rsp+80h], rcx
    call SpiderFixResolveProc
    movaps xmm0, [rsp+88h]
    movaps xmm1, [rsp+98h]
    movaps xmm2, [rsp+0A8h]
    movaps xmm3, [rsp+0B8h]
    mov r10, rax
    add rsp, 0C8h
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
