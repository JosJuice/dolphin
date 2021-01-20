  AREA .text, CODE, READONLY
  ALIGN 4

  EXPORT GetFPCR
  EXPORT SetFPCR

GetFPCR PROC
  mrs x0, fpcr
  ret
  ENDP

SetFPCR PROC
  msr fpcr, x0
  ret
  ENDP

  END
