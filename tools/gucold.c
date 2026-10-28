/* GUCOLD - minimal read-only cold-register reader.
 * Prints the nine cold registers to the console and to CC.TXT on every
 * writable volume, then chain-loads Windows. No GPU writes, no unlock.
 * Purpose: decide whether a *minimal* image can read the same MMIO the
 * full wrapper reports as zero.
 */
#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PciIo.h>
#include <Guid/FileInfo.h>

static EFI_GUID SfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID DpGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID FiGuid = EFI_FILE_INFO_ID;
static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static const UINT32 Offsets[9] = {
    0x000000, 0x118128, 0x118234, 0x110100, 0x1fa828,
    0x823804, 0x82381c, 0x823820, 0x1183a4};
static const CHAR8 *Names[9] = {"chip", "gfw0", "gfw1", "gfw2", "wpr2hi",
                                "plm", "ss0", "ss1", "mem"};
static const UINT8 WindowsGuidBytes[16] = {
    0x29, 0xdc, 0x0d, 0x03, 0xc4, 0x32, 0x9c, 0x42,
    0x88, 0xac, 0x5a, 0xa4, 0xb5, 0xbf, 0x2c, 0x80};

static EFI_SYSTEM_TABLE *gST;
static VOID CP(const CHAR8 *t){CHAR16 line[220];UINTN i=0;
  if(!gST||!gST->ConOut)return;
  while(*t && i<218)line[i++]=(CHAR16)*t++;line[i]=0;
  gST->ConOut->OutputString(gST->ConOut,line);}
static VOID CPH(UINT64 v,UINTN d){static const CHAR8 h[]="0123456789abcdef";
  CHAR8 t[24];UINTN i;if(d>16)d=16;for(i=0;i<d;i++)t[i]=h[(v>>(4*(d-1-i)))&0xf];
  t[d]=0;CP(t);}
static VOID CPD(UINT64 v){CHAR8 t[24];UINTN n=0,i;
  if(!v){CP("0");return;}while(v&&n<sizeof(t)){t[n++]=(CHAR8)('0'+(v%10));v/=10;}
  for(i=n;i>0;i--)CP((CHAR8[]){t[i-1],0});}

static CHAR8 Report[4096];
static UINTN ReportLen;

static VOID Ap(CHAR8 c) { if (ReportLen + 1 < sizeof(Report)) Report[ReportLen++] = c; }
static VOID S(const CHAR8 *s) { while (*s) Ap(*s++); }
static VOID H(UINT64 v, UINTN d) {
  static const CHAR8 h[] = "0123456789abcdef";
  CHAR8 t[20]; UINTN i;
  if (d > 16) d = 16;
  for (i = 0; i < d; i++) t[i] = h[(v >> (4 * (d - 1 - i))) & 0xf];
  for (i = 0; i < d; i++) Ap(t[i]);
}
static VOID D(UINT64 v) {
  CHAR8 t[24]; UINTN n = 0, i;
  if (!v) { Ap('0'); return; }
  while (v && n < sizeof(t)) { t[n++] = (CHAR8)('0' + (v % 10)); v /= 10; }
  for (i = n; i > 0; i--) Ap(t[i - 1]);
}
static BOOLEAN Eq16(const UINT8 *a, const UINT8 *b) {
  UINTN i;
  for (i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
  return 1;
}
static BOOLEAN HdNode(const EFI_DEVICE_PATH_PROTOCOL *dp, const UINT8 **guid) {
  const UINT8 *p = (const UINT8 *)dp;
  UINTN hd = 0;
  *guid = 0;
  if (!dp) return 0;
  for (;;) {
    const EFI_DEVICE_PATH_PROTOCOL *n = (const EFI_DEVICE_PATH_PROTOCOL *)p;
    UINTN len = n->Length[0] | ((UINTN)n->Length[1] << 8);
    if (len < 4) return 0;
    if (n->Type == MEDIA_DEVICE_PATH && n->SubType == MEDIA_HARDDRIVE_DP) {
      const HARDDRIVE_DEVICE_PATH *h = (const HARDDRIVE_DEVICE_PATH *)n;
      if (++hd == 1) *guid = h->Signature;
    }
    if (n->Type == END_DEVICE_PATH_TYPE) break;
    p += len;
  }
  return *guid != 0;
}
static EFI_STATUS WriteReport(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs) {
  EFI_FILE_PROTOCOL *root = 0, *f = 0;
  EFI_STATUS s = fs->OpenVolume(fs, &root);
  if (EFI_ERROR(s)) return s;
  s = root->Open(root, &f, L"CC.TXT",
                 EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR(s) && f) {
    UINT8 info[sizeof(EFI_FILE_INFO) + 64];
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
    UINTN sz = ReportLen, isz = sizeof(info);
    f->Write(f, &sz, Report);
    if (!EFI_ERROR(f->GetInfo(f, &FiGuid, &isz, fi))) {
      fi->FileSize = ReportLen;
      f->SetInfo(f, &FiGuid, isz, fi);
    }
    f->Close(f);
  }
  root->Close(root);
  return s;
}


/* ===== LOCAL: verbatim replication of the wrapper's GuPeekCold logic =====
 * Same device filter, same 16-dword config read, same order, same ColdRead
 * comparison. Run here in the minimal app that is known to read real values,
 * to tell whether the function itself fails or the wrapper context does. */
static EFI_STATUS ColdReadC(EFI_PCI_IO_PROTOCOL *io, UINT32 *values) {
  UINTN i;
  if (!io || !io->Mem.Read) return EFI_UNSUPPORTED;
  for (i = 0; i < 9; i++) {
    EFI_STATUS st = io->Mem.Read(io, EfiPciIoWidthUint32, 0, Offsets[i], 1, &values[i]);
    if (st != EFI_SUCCESS) return st;
  }
  if (values[0] != 0xb74000a1U || values[1] != 0x8b8f || values[2] != 0x3ff ||
      values[3] != 0x10 || values[4] || values[5] != 0xffffff8f ||
      values[6] == 0xffffffff || values[6] == 0x88888888 ||
      (values[6] & 0xffff0000) == 0xbadf0000 || values[7] > 15 ||
      values[8] != 0x2000)
    return EFI_NOT_READY;
  return EFI_SUCCESS;
}
static VOID Phase3(EFI_BOOT_SERVICES *bs, EFI_SYSTEM_TABLE *st) {
  EFI_HANDLE *hs = 0;
  UINTN n = 0, i, found = 0;
  EFI_PCI_IO_PROTOCOL *target = 0;
  UINT32 targetConfig[16];
  UINT32 vals[9];
  UINTN j;
  EFI_STATUS s = bs->LocateHandleBuffer(ByProtocol, &PciGuid, 0, &n, &hs);
  UINT32 stage = 0;
  if (s != EFI_SUCCESS) { CP("phase3: enum failed\r\n"); return; }
  for (i = 0; i < n; i++) {
    EFI_PCI_IO_PROTOCOL *io = 0; UINTN seg = 0, bus = 0, dev = 0, fun = 0; UINT32 cfg[16];
    if (EFI_ERROR(bs->HandleProtocol(hs[i], &PciGuid, (VOID **)&io)) || !io) continue;
    if (!io->GetLocation || !io->Pci.Read) continue;
    if (io->GetLocation(io, &seg, &bus, &dev, &fun) != EFI_SUCCESS || seg || bus != 1 ||
        dev || fun) continue;
    if (io->Pci.Read(io, EfiPciIoWidthUint32, 0, 16, cfg) != EFI_SUCCESS) continue;
    if (cfg[0] != 0x24c910deU || cfg[11] != 0x16fb10deU || (cfg[2] >> 24) != 3) continue;
    found++; target = io;
    for (j = 0; j < 16; j++) targetConfig[j] = cfg[j];
  }
  if (hs) bs->FreePool(hs);
  if (found != 1) { CP("phase3: found!="); CPD(found); CP("\r\n"); return; }
  if ((targetConfig[1] & 6U) != 2U || targetConfig[4] != 0x80000000U) {
    CP("phase3: cfg gate failed cmd=0x"); CPH(targetConfig[1], 8);
    CP(" bar0=0x"); CPH(targetConfig[4], 8); CP("\r\n"); return;
  }
  for (j = 0; j < 9; j++) vals[j] = 0;
  EFI_STATUS cr = ColdReadC(target, vals);
  stage = 4;
  CP("phase3: ColdRead status=0x"); CPH((UINT64)cr, 16);
  CP(" stage="); CPD(stage); CP("\r\n");
  for (j = 0; j < 9; j++) {
    CP("  "); CP(Names[j]); CP(" val=0x"); CPH(vals[j], 8); CP("\r\n");
  }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *st) {
  EFI_BOOT_SERVICES *bs = st->BootServices;
  EFI_HANDLE *handles = 0;
  UINTN count = 0, i;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *windowsFs = 0;
  EFI_STATUS s;
  UINT32 vals[9];
  UINTN hits = 0;
  EFI_STATUS readStatus[9];
  UINTN j;
  EFI_PCI_IO_PROTOCOL *savedIo = 0;

  gST = st;
  ReportLen = 0;
  for (j = 0; j < 9; j++) { vals[j] = 0xdeadbeef; readStatus[j] = EFI_NOT_READY; }

  S("GUCOLD v1 read-only\r\n\r\n");

  s = bs->LocateHandleBuffer(ByProtocol, &PciGuid, 0, &count, &handles);
  S("pci handles: ");
  D(EFI_ERROR(s) ? 0 : count);
  S("\r\n");
  if (!EFI_ERROR(s)) {
    for (i = 0; i < count; i++) {
      EFI_PCI_IO_PROTOCOL *io = 0;
      UINTN seg = 0, bus = 0, dev = 0, fun = 0;
      UINT32 id = 0;
      if (EFI_ERROR(bs->HandleProtocol(handles[i], &PciGuid, (VOID **)&io)) || !io) continue;
      if (!io->Pci.Read || !io->GetLocation || !io->Mem.Read) continue;
      if (EFI_ERROR(io->Pci.Read(io, EfiPciIoWidthUint32, 0, 1, &id))) continue;
      if (id != 0x24c910deU) continue;
      if (EFI_ERROR(io->GetLocation(io, &seg, &bus, &dev, &fun))) continue;
      S("target: id="); H(id, 8);
      S(" seg="); D(seg); S(" bus="); D(bus);
      S(" dev="); D(dev); S(" fun="); D(fun);
      S("\r\n");
      hits++;
      savedIo = io;
      for (j = 0; j < 9; j++) {
        UINT32 v = 0;
        readStatus[j] = io->Mem.Read(io, EfiPciIoWidthUint32, 0, Offsets[j], 1, &v);
        vals[j] = v;
      }
      /* LOCAL experiment: now perform the 16-dword config read the wrapper does,
       * then read MMIO again. If the second set is zero, the config read is the
       * cause; if both are equal, it is not. */
      {
        UINT32 cfg16[16];
        EFI_STATUS cfgStatus = io->Pci.Read(io, EfiPciIoWidthUint32, 0, 16, cfg16);
        S("cfg16 st=0x"); H((UINT64)cfgStatus, 16);
        S(" id=0x"); H(cfg16[0], 8);
        S(" cmd=0x"); H(cfg16[1], 8);
        S(" bar0=0x"); H(cfg16[4], 8);
        S("\r\n");
      }
      S("-- after 16-dword config read --\r\n");
      for (j = 0; j < 9; j++) {
        UINT32 v = 0;
        EFI_STATUS st2 = io->Mem.Read(io, EfiPciIoWidthUint32, 0, Offsets[j], 1, &v);
        S("  "); S(Names[j]);
        S(" st=0x"); H((UINT64)st2, 16);
        S(" val=0x"); H(v, 8);
        S("\r\n");
      }
    }
  }
  if (handles) bs->FreePool(handles);

  /* LOCAL experiment: the wrapper frees the handle buffer before reading MMIO.
   * Read again after that free to see whether it is what zeroes the window. */
  if (savedIo) {
    S("-- after FreePool(handle buffer) --\r\n");
    for (j = 0; j < 9; j++) {
      UINT32 v = 0;
      EFI_STATUS st3 = savedIo->Mem.Read(savedIo, EfiPciIoWidthUint32, 0, Offsets[j], 1, &v);
      S("  "); S(Names[j]);
      S(" st=0x"); H((UINT64)st3, 16);
      S(" val=0x"); H(v, 8);
      S("\r\n");
    }
    S("\r\n");
  }

  if (savedIo) {
    /* LOCAL experiment: the wrapper writes its log file before reading MMIO.
     * Create and write a file here, then read the registers again. */
    EFI_HANDLE *sh = 0;
    UINTN sn = 0;
    if (!EFI_ERROR(bs->LocateHandleBuffer(ByProtocol, &SfsGuid, 0, &sn, &sh)) && sh) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *wfs = 0;
      if (!EFI_ERROR(bs->HandleProtocol(sh[0], &SfsGuid, (VOID **)&wfs))) {
        EFI_FILE_PROTOCOL *root = 0, *wf = 0;
        if (!EFI_ERROR(wfs->OpenVolume(wfs, &root))) {
          if (!EFI_ERROR(root->Open(root, &wf, L"WTEST.TXT",
                                    EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE, 0)) && wf) {
            CHAR8 msg[32] = "write test 0123456789";
            UINTN mn = 24;
            wf->Write(wf, &mn, msg);
            wf->Flush(wf);
            wf->Close(wf);
          }
          root->Close(root);
        }
      }
      bs->FreePool(sh);
    }
    S("-- after file write --\r\n");
    for (j = 0; j < 9; j++) {
      UINT32 v = 0;
      EFI_STATUS st5 = savedIo->Mem.Read(savedIo, EfiPciIoWidthUint32, 0, Offsets[j], 1, &v);
      S("  "); S(Names[j]);
      S(" st=0x"); H((UINT64)st5, 16);
      S(" val=0x"); H(v, 8);
      S("\r\n");
    }
    S("\r\n");
  }

  S("hits: "); D(hits); S("\r\n\r\n");
  for (j = 0; j < 9; j++) {
    S("  "); S(Names[j]);
    S(" off=0x"); H(Offsets[j], 8);
    S(" st=0x"); H((UINT64)readStatus[j], 16);
    S(" val=0x"); H(vals[j], 8);
    S("\r\n");
  }

  /* Second SFS pass: write the report to every writable volume, remember the ESP. */
  s = bs->LocateHandleBuffer(ByProtocol, &SfsGuid, 0, &count, &handles);
  if (!EFI_ERROR(s)) {
    for (i = 0; i < count; i++) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
      EFI_DEVICE_PATH_PROTOCOL *dp = 0;
      const UINT8 *guid = 0;
      if (EFI_ERROR(bs->HandleProtocol(handles[i], &SfsGuid, (VOID **)&fs))) continue;
      bs->HandleProtocol(handles[i], &DpGuid, (VOID **)&dp);
      if (HdNode(dp, &guid) && Eq16(guid, WindowsGuidBytes)) { windowsFs = fs; }
      WriteReport(fs);
    }
    bs->FreePool(handles);
  }

  /* Console print. */
  {
    CHAR16 line[160];
    UINTN n = 0, k = 0;
    for (n = 0; n <= ReportLen; n++) {
      CHAR8 c = (n < ReportLen) ? Report[n] : '\n';
      if (c == '\r') continue;
      if (c == '\n' || k >= 158) {
        line[k++] = L'\r'; line[k++] = L'\n'; line[k] = 0;
        st->ConOut->OutputString(st->ConOut, line);
        k = 0;
        if (c == '\n') continue;
      }
      line[k++] = (CHAR16)c;
    }
  }

  Phase3(bs, st);

  /* Chain-load Windows Boot Manager so the machine returns to Windows. */
  if (windowsFs) {
    EFI_FILE_PROTOCOL *root = 0, *f = 0;
    if (!EFI_ERROR(windowsFs->OpenVolume(windowsFs, &root))) {
      if (!EFI_ERROR(root->Open(root, &f, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
                                EFI_FILE_MODE_READ, 0)) && f) {
        EFI_FILE_INFO *fi = 0;
        UINTN isz = 0;
        f->GetInfo(f, &FiGuid, &isz, 0);
        if (isz) {
          bs->AllocatePool(EfiLoaderData, isz, (VOID **)&fi);
          if (fi && !EFI_ERROR(f->GetInfo(f, &FiGuid, &isz, fi))) {
            VOID *buf = 0;
            bs->AllocatePool(EfiLoaderData, (UINTN)fi->FileSize, &buf);
            if (buf) {
              UINTN rsz = (UINTN)fi->FileSize;
              EFI_HANDLE img = 0;
              if (!EFI_ERROR(f->Read(f, &rsz, buf)) &&
                  !EFI_ERROR(bs->LoadImage(0, ImageHandle, 0, buf, rsz, &img))) {
                /* LOCAL experiment: the wrapper LoadImage()s bootmgfw before its
                 * cold check. Read once more here, after that load, before start. */
                UINTN k;
                S("-- after LoadImage(bootmgfw) --\r\n");
                for (k = 0; k < 9; k++) {
                  UINT32 v = 0;
                  EFI_STATUS st4 = savedIo
                                       ? savedIo->Mem.Read(savedIo, EfiPciIoWidthUint32,
                                                           0, Offsets[k], 1, &v)
                                       : EFI_NOT_READY;
                  S("  "); S(Names[k]);
                  S(" st=0x"); H((UINT64)st4, 16);
                  S(" val=0x"); H(v, 8);
                  S("\r\n");
                }
                S("\r\n");
                UINTN ec = 0;
                bs->StartImage(img, &ec, 0);
              }
            }
          }
        }
        f->Close(f);
      }
      root->Close(root);
    }
  }
  return EFI_SUCCESS;
}
