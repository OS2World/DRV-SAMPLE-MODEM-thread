/******************************************************************************/
/*                                                                            */
/*                                                                            */
/*      Sample C Multi-Thread application                                     */
/*                                                                            */
/*      Written: 1/16/88                                                      */
/*      Author : Sam Detweiler                                                */
/*                                                                            */
/*      Function: Any keys pressed will be passed UNFILTERED to the           */
/*                async device driver. If you have a smart modem              */
/*                you may dial phone numbers, or whatever.                    */
/*                                                                            */
/*                A great display of smooth screen update is                  */
/*                HELP on IBM interal 1200 bauds modem                       */
/*                                                                            */
/*      The threads provide the following services                            */
/*                                                                            */
/*      1. comthread() reads from the async device driver                     */
/*         into a circular buffer                                             */
/*         if no characters are in the buffer                                 */
/*         a four character time DosSleep is executed                         */
/*                                                                            */
/*                                                                            */
/*      2. kbdthread() reads the keyboard via KbdCharin (wait)                */
/*         it processes both keystrokes and shift state changes               */
/*                                                                            */
/*         the keystroke record is read into a circular buffer                */
/*         so that it need not be moved again.                                */
/*                                                                            */
/*      both threads 1 & 2 will wait on semiphores if their respective        */
/*      circular buffers have become full.                                    */
/*                                                                            */
/*      3. Main thread                                                        */
/*                                                                            */
/*         opens "COM1"                                                       */
/*         sets device operation characteristics                              */
/*         sets baud rate                                                     */
/*         sets line characteristics                                          */
/*         sets keyboard to binary, shirt report on                           */
/*         allocates circular buffers for keyboard and comm data              */
/*         initializes shift state display status string                      */
/*         starts threads for com and kbd processing                          */
/*                                                                            */
/*         does a DosMuxSemWait                                               */
/*                on two semiphores                                           */
/*                   1 - com thread data in buffer                            */
/*                   2 - kbd thread data in buffer                            */
/*                       if the keystroke is Ctrl-Z breaks loop               */
/*                                                                            */
/*                                                                            */
/*                                                                            */
/*                                                                            */
/******************************************************************************/
#define INCL_DOS
#define INCL_KBD
#define INCL_VIO
#include <os2.h>                /* include dos function declarations    */
#include <stdlib.h>             /* include C memory mgmt defines        */
#include <stdio.h>              /* include C memory mgmt defines        */
#include <malloc.h>             /* include C memory mgmt defines        */

HFILE handle;                     /* COM1 file handle after open          */

USHORT DONE=0;                /* thread spin flag until <>0           */

#define KeySize sizeof(KBDKEYINFO) /* size of keystroke data record */

char keystates[18];             /* shift status report string           */
char keymask[]="ICNSAcLR";      /* mask of shift state flags            */

                /* pointers to circular buffers */
                /* _start is address of first entry     */
                /* _end   is address of last  entry     */
                /* _last  is address of last entry removed    */
                /* _last is manipulated ONLY by main thread   */
                /* _next  is address of next position to add a record  */
                /* _last is manipulated ONLY by com and kbd threads   */

#define COMM_BUF_ENTRIES     2000   /* max entries in comm circular buffer */
#define KEY_BUF_ENTRIES      200   /* max entries in keyboard circular buffer */
#define THREAD_STACKSIZE 4096      /* size of thread program stack */

                /* for communications buffer */
PCHAR combuf_start,combuf_next,combuf_last,combuf_end;

                /* for keyboard data record buffer */
KBDKEYINFO *keybuf_start,*keybuf_next,*keybuf_last,*keybuf_end;

VIOMODEINFO md;                             /* current video mode data */

struct {char c,a;} attr;        /* char/attribute pair for VIO calls    */

                                /* default line baud rate  */
struct { ULONG rate; char fraction;} mr={2400,0};

char lctrl[3]={8,0,0};          /* line control string                  */
                                /* 8 data bits, no parity, 1 stop       */
char mo[2]={3,0xfd};            /* ENABLE DTR/RTS mode operation data   */

                                /* semiphores for data in buffers       */
                         /* when DosSemCleared will wake up main thread */
HSEM data_in_com_buf_sem,key_in_buf_sem;

                                /* semiphores for data buffers full     */
                   /* when DosSemCleared will wake up respective thread */
HSEM  com_buf_full_sem,key_buf_full_sem;

struct sem_entry { USHORT reserved;           /* structure required by */
                   HSEM sem_handle;};  /* DosMuxSemWait for each sem */

                                /* DosMuxSemWait structure for main thread */
struct { USHORT sem_count;
         struct sem_entry sems[2];} semlist = {
                2,
                0,(HSEM)&data_in_com_buf_sem,
                0,(HSEM)&key_in_buf_sem};

/******************************************************************************/
/*                                                                            */
/*      update shift state display line                                       */
/*                                                                            */
/******************************************************************************/
process_shiftstates(x)
int x;
{
int m,i;
            for(i=0,m=0x0080;i<8;i++,m>>=1)     /* loop thru low bits */
              {
              if(x & m)                         /* bit on ? */
                keystates[i*2]=keymask[i];      /* yes set indicator */
              else
                keystates[i*2]=' ';             /* no, clear indicator */
              }

            VioWrtCharStr(keystates,17,md.row,0,0); /* write indicator string */
}
/******************************************************************************/
/*                                                                            */
/*      Communications line handler thread                                    */
/*                                                                            */
/*      Reads data from Async Device driver receive buffer to local           */
/*      application circular buffer                                           */
/*                                                                            */
/*      operation:                                                            */
/*                                                                            */
/*         do forever until DONE<>0                                           */
/*            DosRead as much as room in buffer for                           */
/*                      (last to end)                                         */
/*                          OR (in case of buffer wrap)                       */
/*                      (last to next)                                        */
/*              if any bytes read                                             */
/*               update next pointer with size read                           */
/*               check for buffer wrap                                        */
/*               DosSemClear main thread com_data_in_buf_sem semiphore        */
/*               if buffer full DoSemSetWait for main thread to take some data*/
/*              else                                                          */
/*               DosSleep 4 character times                                   */
/*              (this sleep should be adjusted for line speed, protocol,      */
/*               buffer size, screen update process, etc to insure max        */
/*               responsiveness without jerky screen updates or system        */
/*               overheard) (try setting this numer to some value >100L       */
/*               for a 1200 baud line and watch the results)                  */
/*                                                                            */
/*         if DONE<>0                                                         */
/*            DosExit thread                                                  */
/*                                                                            */
/******************************************************************************/
comthread()
{
USHORT bytesread;
SHORT  len;

        DosSetPrty(PRTYS_THREAD,PRTYC_TIMECRITICAL,PRTYD_MINIMUM,0);
        for(;!DONE;)                    /* loop til DONE <> 0 */
          {
          if(combuf_next>=combuf_last) /* check for remaining space in buffer*/
            len=combuf_end-combuf_next;/* _last leading _next pointer */
          else
            len=combuf_last-combuf_next;/* _last trailing _next pointer */

                                        /* read as much as possible */
          DosRead(handle,combuf_next,len,&bytesread);

          if(bytesread)                 /* make sure we actually read some */
            {
            combuf_next+=bytesread;   /* update _next pointer for data read */

            if(combuf_next>=combuf_end) /* if the buffer would wrap */
              combuf_next=combuf_start; /* start at the front again */


            if(combuf_next==combuf_last) /* if buffer full */
              {
                                     /* set semaphore to waiy for main thread */
              DosSemSet((HSEM)&com_buf_full_sem);
                                     /* tell main thread data is available */
              DosSemClear((HSEM)&data_in_com_buf_sem);
                                         /* wait til main thread takes some */
              DosSemWait((HSEM)&com_buf_full_sem,-1L);
              }
            else
                                     /* tell main thread data is available */
              DosSemClear((HSEM)&data_in_com_buf_sem);
            }
          }
        DosExit(0,0);                      /* DONE<>0 end thread */
}

/******************************************************************************/
/*                                                                            */
/*      Keyboard buffer handler thread                                        */
/*                                                                            */
/*      Reads keystrokes and shift report records into local                  */
/*      application circular buffer                                           */
/*                                                                            */
/*      operation:                                                            */
/*                                                                            */
/*         do forever until DONE<>0                                           */
/*            KbdCharIn WAIT  for next keystroke/shift report                 */
/*            if shift report (shows ONLY CHANGES since last report)          */
/*              update user awareness string                                  */
/*              write user awareness string to reserved last line of display  */
/*            else                                                            */
/*              if keystroke                                                  */
/*                update _next pointer                                        */
/*                check for buffer wrap                                       */
/*                                                                            */
/*         if DONE<>0                                                         */
/*            DosExit thread                                                  */
/*                                                                            */
/******************************************************************************/
kbdthread()
{
int rc,i;

        for(;!DONE;)               /* loop in this thread til main says done */
          {
          KbdCharIn(keybuf_next,0,0);       /* get keystroke or status, wait */
          if(keybuf_next->fbStatus &0x01)         /* shift status change */
                                                /* update shift state display */
            process_shiftstates(keybuf_next->fsState);

          else
            if(keybuf_next->fbStatus &0x40)       /* character returned */
              {
              keybuf_next++;                    /* say we had one */

              if(keybuf_next>=keybuf_end)       /* buffer wrap ? */
                keybuf_next=keybuf_start;       /* point to start */

                                                /* tell handler some data */
                DosSemClear((HSEM)&key_in_buf_sem);

              if(keybuf_next==keybuf_last)      /* about to overlay start? */
                {
                                                /* so wait til some removed */
                DosSemSet((HSEM)&key_buf_full_sem);
                                                /* tell handler some data */
                DosSemClear((HSEM)&key_in_buf_sem);
                                                /* so wait til some removed */
                DosSemWait((HSEM)&key_buf_full_sem,-1L);
                }
              else
                                                /* tell handler some data */
                DosSemClear((HSEM)&key_in_buf_sem);

              }
          }
        DosExit(0,0);                           /* DONE<>0 end thread */
}

/******************************************************************************/
/*                                                                            */
/*      insures last line (shift state disply line) will not                  */
/*      be overwritten by data                                                */
/*                                                                            */
/******************************************************************************/
protect_lastline()
{
int cursor_row,cursor_col;
        VioGetCurPos(&cursor_row,&cursor_col,0); /* get cursor position */
        if(cursor_row==md.row)                   /* on last line? */
          {                                      /* yes, scroll screen 1 line */
          VioScrollUp(0,0,md.row-1,md.col,1,(char far *)&attr,0);
                                                 /* move cursor up one line */
          VioSetCurPos(cursor_row-1,cursor_col,0);
          }
}
/******************************************************************************/
/*                                                                            */
/*      Main Thread                                                           */
/*                                                                            */
/*      does ALL OUTPUT and INPUT processing                                  */
/*      writes to display and async device driver                             */
/*                                                                            */
/*                                                                            */
/*                                                                            */
/******************************************************************************/
main(int argc, char *argv[])
{
int act,br,rc,i,ce,keytid,comtid,sem_index,cursor_row,cursor_col;
unsigned m;
char r,ch,*comstack,*keystack;
KBDKEYINFO y;
KBDINFO kbstat;
char *comname;
struct {
       unsigned write_timeout,read_timeout;
       unsigned char flags1,
                     flags2,
                     flags3,
                     error_char,
                     break_char,
                     xon_char,
                     xoff_char;
       } dcb;
                        /*                                              */
                        /*   Open COM1, if is exists, no sharing        */
                        /*                                              */
        if(argc>1)
          comname=argv[1];
        else
          comname="COM2";
        rc=DosOpen(comname,&handle,&act,0L,0,0x01,0x92,0L);

                        /* get device characteristics block             */
        rc=DosDevIOCtl((char far *)&dcb,0L,0x73,1,handle);
                        /* set read and write timeout as small as possible */
        dcb.read_timeout=20000;
        dcb.write_timeout=0;
                        /* Turn on DTR       */
        dcb.flags1 = 0x01;

                        /* Turn on RTS       */
        dcb.flags2 = 0x40;

                        /* Set NOWAIT read processing   */
        dcb.flags3 = 0x04;

                        /* update device characteristics                   */
        rc=DosDevIOCtl(0L,(char far *)&dcb,0x53,1,handle);

        if(_osmajor>10)
          {
          if(argc>2)
            {
            mr.rate=atol(argv[2]);              /* Set Baud rate                                   */
            } /* end if */
          rc=DosDevIOCtl(0L,(char far *)&mr,mr.rate>19200?0x43:0x41,1,handle);
          }
        else
          {
                        /* Set Baud rate                                   */
          rc=DosDevIOCtl(0L,(char far *)&mr,0x41,1,handle);
          }

                        /* Set 8 data bits, No parity, 1 stop bit          */
        rc=DosDevIOCtl(0L,(char far *)lctrl,0x42,1,handle);

                        /* Make sure DTR and RTS are on                    */
        rc=DosDevIOCtl((char far *)&ce,(char far *)mo,0x46,1,handle);

                        /* allocate keystoke circular buffer */
        if(!(keybuf_start=keybuf_next=keybuf_last=malloc(KEY_BUF_ENTRIES*KeySize)))
           exit(printf("Out of storage kbdbuf\n"));

                        /* allocate communicaition circular buffer */
        if(!(combuf_start=combuf_next=combuf_last=malloc(COMM_BUF_ENTRIES)))
           exit(printf("Out of storage combuf\n"));

        combuf_end=combuf_start+COMM_BUF_ENTRIES;/* set end of buffer pointers */
        keybuf_end=keybuf_start+KEY_BUF_ENTRIES;

        attr.a=0x07;                    /* set scroll attribute */
        attr.c=' ';                     /* set scroll data byte */
        VioScrollUp(0,0,-1,-1,-1,(char far *)&attr,0); /* clear screen */

        VioSetCurPos(0,0,0);            /* set cursor, top left corner */

        md.cb=12;                       /* set mode data length */
        rc=VioGetMode(&md,0);           /* get video mode data */
        md.row--;                       /* make 0 relative */
        md.col--;                       /* make 0 realtive */

        kbstat.cb=10;                   /* keyboard status data length */
        rc=KbdGetStatus(&kbstat,0);     /* get keyboard status data */

        kbstat.fsMask=0x0106;           /* set shift report on, Binary (raw mode) */

        memset(keystates,' ',17);       /* clear shift status line */

        process_shiftstates(kbstat.fsState);/* update shift state display */

        rc=KbdSetStatus(&kbstat,0);     /* set keyboard status now */

                                        /* set MuxSemWait semiphores */
        DosSemSet((HSEM)&data_in_com_buf_sem);
        DosSemSet((HSEM)&key_in_buf_sem);

                                        /* create kbd thread */
        _beginthread(kbdthread,NULL,THREAD_STACKSIZE,NULL);

                                        /* create com thread */
        _beginthread(comthread,NULL,THREAD_STACKSIZE,NULL);

        printf("Enter your Modem commands now (NoWait Mode)\n");

        for(;;)
          {                   /* wait for one of two semiphores to be cleared */
          DosMuxSemWait((unsigned far *)&sem_index,(unsigned far *)&semlist,-1L);

          switch(sem_index)   /* semindex tells which one cleared */
             {
             case 0:          /* com data in buffer */

                  protect_lastline();  /* make sure we don't write on last line */
                                           /* reset semiphore so we will wait */
                  DosSemSet((HSEM)&data_in_com_buf_sem);

                  VioWrtTTY(combuf_last++,1,0); /* write data to screen */

                  if(combuf_last>=combuf_end)   /* buffer wrap? */
                    combuf_last=combuf_start;   /* point to start */

                  if(combuf_last!=combuf_next)  /* buffer empty? */
                                       /* NO, reset semiphore so we don' wait */
                    DosSemClear((HSEM)&data_in_com_buf_sem);

                                              /* clear buffer full semiphore */
                                             /* in case the thread is waiting */
                  DosSemClear((HSEM)&com_buf_full_sem);

                  break;                        /* done */
             case 1:            /* keystroke in buffer */
                                /* get then character code */
                  ch=keybuf_last++->chChar;

                  if(ch==0x1a)  /* is it Ctrl-Z */
                    break;      /* yes, end processing */

                  protect_lastline();  /* make sure we don't write on last line */

                  VioWrtTTY(&ch,1,0);  /* write data to screen */

                  rc=DosWrite(handle,&ch,1,&br);  /* write data to device */

                  if(keybuf_last>=keybuf_end)     /* buffer wrap ? */
                    keybuf_last=keybuf_start;     /* set back to start */

                  if(keybuf_last==keybuf_next)    /* buffer empty? */
                                           /* reset semiphore so we will wait */
                    DosSemSet((HSEM)&key_in_buf_sem);

                                              /* clear buffer full semiphore */
                                             /* in case the thread is waiting */
                  DosSemClear((HSEM)&key_buf_full_sem);

                  break;                        /* keyboard done */

             default:                           /* SHOULDN'T get here */
                  printf("oops, index is %d\n",sem_index);
                  break;
             }
          if(ch==0x1a)                          /* Ctrl-Z keystroke? */
             break;                             /* end loop */
          }
        DONE=1;                                 /* set done <> 0 */
        DosClose(handle);                       /* close COM1 */
        DosExit(1,0);                           /* and exit */
}
