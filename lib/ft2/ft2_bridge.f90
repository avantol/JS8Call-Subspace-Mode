module ft2_bridge
! Bridge module: wraps OOP ft2_decoder with flat C-callable functions.
! This allows C++ code to call the Fortran FT2 encoder and decoder
! without needing to understand Fortran derived types or callbacks.

  use iso_c_binding
  use ft2_decode
  implicit none

  ! C callback function pointer type
  abstract interface
    subroutine c_callback_iface(sync, snr, dt, freq, msg, msglen, &
         nap, qual, msgbits77, ctx) bind(C)
      use iso_c_binding
      real(c_float), value, intent(in) :: sync
      integer(c_int), value, intent(in) :: snr
      real(c_float), value, intent(in) :: dt
      real(c_float), value, intent(in) :: freq
      character(kind=c_char), intent(in) :: msg(*)
      integer(c_int), value, intent(in) :: msglen
      integer(c_int), value, intent(in) :: nap
      real(c_float), value, intent(in) :: qual
      integer(c_int8_t), intent(in) :: msgbits77(77)
      type(c_ptr), value, intent(in) :: ctx
    end subroutine c_callback_iface
  end interface

  ! Module-level state for callback forwarding
  type(c_funptr), save :: g_c_callback
  type(c_ptr), save :: g_c_ctx

  ! Module-level decoder instance
  type, extends(ft2_decoder) :: bridge_ft2_decoder
    integer :: decoded
  end type bridge_ft2_decoder

  type(bridge_ft2_decoder), save :: g_decoder

contains

  ! Fortran callback trampoline: called by ft2_decoder%decode,
  ! forwards to the C function pointer
  subroutine bridge_callback(this, sync, snr, dt, freq, decoded, nap, &
                             qual, msgbits77)
    class(ft2_decoder), intent(inout) :: this
    real, intent(in) :: sync
    integer, intent(in) :: snr
    real, intent(in) :: dt
    real, intent(in) :: freq
    character(len=37), intent(in) :: decoded
    integer, intent(in) :: nap
    real, intent(in) :: qual
    integer*1, intent(in) :: msgbits77(77)

    procedure(c_callback_iface), pointer :: cfun

    select type(this)
    type is (bridge_ft2_decoder)
      this%decoded = this%decoded + 1
    end select

    call c_f_procpointer(g_c_callback, cfun)
    if(.not. associated(cfun)) return

    call cfun(real(sync, c_float), int(snr, c_int), &
         real(dt, c_float), real(freq, c_float), &
         decoded, int(37, c_int), &
         int(nap, c_int), real(qual, c_float), &
         msgbits77, g_c_ctx)
  end subroutine bridge_callback

  ! Main decode entry point, callable from C
  subroutine ft2_decode_c(iwave, nmax, nfqso, nfa, nfb, ndepth, &
       callback, ctx) bind(C, name='ft2_decode_c')
    integer(c_int16_t), intent(in) :: iwave(*)
    integer(c_int), value, intent(in) :: nmax
    integer(c_int), value, intent(in) :: nfqso
    integer(c_int), value, intent(in) :: nfa
    integer(c_int), value, intent(in) :: nfb
    integer(c_int), value, intent(in) :: ndepth
    type(c_funptr), value, intent(in) :: callback
    type(c_ptr), value, intent(in) :: ctx

    character(len=12) :: mycall, hiscall
    integer :: nQSOProgress, ncontest
    logical :: lapcqonly

    ! Store callback info for trampoline
    g_c_callback = callback
    g_c_ctx = ctx
    g_decoder%decoded = 0

    ! Default parameters (can be extended later)
    nQSOProgress = 0
    lapcqonly = .false.
    ncontest = 0
    mycall = '            '
    hiscall = '            '

    ! Call the decoder
    call g_decoder%decode(bridge_callback, iwave, nQSOProgress, &
         nfqso, nfa, nfb, ndepth, lapcqonly, ncontest, &
         mycall, hiscall)

  end subroutine ft2_decode_c

  ! Encode a message to FT2 tones
  subroutine ft2_encode_c(msg, i4tone, msgsent) &
       bind(C, name='ft2_encode_c')
    character(kind=c_char), intent(in) :: msg(37)
    integer(c_int), intent(out) :: i4tone(103)
    character(kind=c_char), intent(out) :: msgsent(37)

    character(len=37) :: fmsg, fmsgsent
    integer*1 :: msgbits(77)
    integer :: ichk, i

    ! Convert C chars to Fortran string
    do i = 1, 37
      fmsg(i:i) = msg(i)
    end do

    ichk = 0
    call genft2(fmsg, ichk, fmsgsent, msgbits, i4tone)

    ! Convert Fortran string back to C chars
    do i = 1, 37
      msgsent(i) = fmsgsent(i:i)
    end do

  end subroutine ft2_encode_c

  ! Generate GFSK waveform from tones
  subroutine ft2_gen_wave_c(i4tone, nsym, nsps, fsample, f0, &
       wave, nwave) bind(C, name='ft2_gen_wave_c')
    integer(c_int), intent(in) :: i4tone(*)
    integer(c_int), value, intent(in) :: nsym
    integer(c_int), value, intent(in) :: nsps
    real(c_float), value, intent(in) :: fsample
    real(c_float), value, intent(in) :: f0
    real(c_float), intent(out) :: wave(*)
    integer(c_int), value, intent(in) :: nwave

    integer :: lnsym, lnsps, lnwave, licmplx
    real :: lfsample, lf0
    complex :: cwave_dummy(1)

    lnsym = nsym
    lnsps = nsps
    lfsample = fsample
    lf0 = f0
    licmplx = 0
    lnwave = nwave

    call gen_ft2wave(i4tone, lnsym, lnsps, lfsample, lf0, &
         cwave_dummy, wave, licmplx, lnwave)

  end subroutine ft2_gen_wave_c

  ! Clear multi-period averaging state
  subroutine ft2_clravg_c() bind(C, name='ft2_clravg_c')
    call ft2_clravg()
  end subroutine ft2_clravg_c

  ! Encode raw 77 message bits to FT2 tones (bypasses pack77)
  subroutine ft2_encode_from_bits_c(msgbits_in, i4tone) &
       bind(C, name='ft2_encode_from_bits_c')
    integer(c_int8_t), intent(in) :: msgbits_in(77)
    integer(c_int), intent(out) :: i4tone(103)

    integer*1 :: msgbits(91)  ! 91: encode174_91 writes CRC past bit 77
    integer :: i

    msgbits = 0
    do i = 1, 77
      msgbits(i) = msgbits_in(i)
    end do

    call get_ft2_tones_from_77bits(msgbits, i4tone)
  end subroutine ft2_encode_from_bits_c

  ! Level 2 sync-triggered decoder, callable from C
  ! Returns raw message77 bits for each decode (up to 20)
  subroutine ft2_triggered_decode_c(iwave, nfqso, nfa, nfb, ndepth, &
       snr_out, dt_out, freq_out, msgbits_out, ndecoded) &
       bind(C, name='ft2_triggered_decode_c')
    integer(c_int16_t), intent(in) :: iwave(*)
    integer(c_int), value, intent(in) :: nfqso
    integer(c_int), value, intent(in) :: nfa
    integer(c_int), value, intent(in) :: nfb
    integer(c_int), value, intent(in) :: ndepth
    integer(c_int), intent(out) :: snr_out(20)
    real(c_float), intent(out) :: dt_out(20)
    real(c_float), intent(out) :: freq_out(20)
    integer(c_int8_t), intent(out) :: msgbits_out(77, 20)
    integer(c_int), intent(out) :: ndecoded

    character(len=12) :: mycall, hiscall
    integer :: nQSOProgress, ncontest

    nQSOProgress = 0
    ncontest = 0
    mycall = '            '
    hiscall = '            '

    call ft2_triggered_decode(iwave, nQSOProgress, nfqso, nfa, nfb, &
         ndepth, ncontest, mycall, hiscall, &
         snr_out, dt_out, freq_out, msgbits_out, ndecoded)

  end subroutine ft2_triggered_decode_c

  ! Initialize FFTW patience common block
  subroutine ft2_init_c() bind(C, name='ft2_init_c')
    common/patience/npatience,nthreads
    integer :: npatience, nthreads
    npatience = 0   ! FFTW_ESTIMATE
    nthreads = 1
  end subroutine ft2_init_c

end module ft2_bridge
