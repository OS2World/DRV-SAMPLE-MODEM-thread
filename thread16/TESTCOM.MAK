testcom.exe:    testcom.obj
                link testcom,,,os2+llibcmt/pm:vio/nod/map;

testcom.obj:    testcom.c
                cl /c /Alfu /G2s /DMT testcom.c

