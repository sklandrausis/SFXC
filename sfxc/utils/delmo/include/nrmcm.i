!@This is the start of file &NRMCM
!
!  NRMPAD PROTECTS AGAINTS VIS PAGING BUGS
!
      REAL*8 SCAL(MAX_PAR),SIG(MAX_PAR),B(MAX_PAR),A(JMAX_TRI)
      REAL*8 NRMPAD(256)
      COMMON/NORM/SCAL,SIG,B,A,NRMPAD
!