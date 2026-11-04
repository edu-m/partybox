@echo -off
# startup.nsh for a UEFI Shell placed next to doom.efi (e.g. PlayDoom.efi):
# finds the volume holding the game, loads the HD Audio driver if it's there
# (digital sound; the PC speaker is used otherwise) and runs doom.efi.
# Add switches to the doom.efi line, e.g. doom.efi -mousescale 3500 -fps
for %a run (0 15)
  if exist fs%a:\EFI\PARTYBOX\PARTYBOX\doom\doom.efi then
    echo "[PlayDoom] found doom on fs%a:, starting doom.efi"
    fs%a:
    cd \EFI\PARTYBOX\PARTYBOX\doom
    if exist drivers\AudioDxe.efi then
      load drivers\AudioDxe.efi
    endif
    doom.efi
    echo "[PlayDoom] doom.efi exited with %lasterror%"
    pause
    exit
  endif
endfor
echo "[PlayDoom] doom.efi not found on fs0..fs15"
pause
