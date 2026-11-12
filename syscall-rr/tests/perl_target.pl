#!/usr/bin/perl
# A Perl program (the interpreter is in every Debian image): files, a child
# through a pipe, a TCP connection to itself, sorting, the clock.  One thread.
# Every line depends on recorded random bytes and the uptime file.  Not on
# time(): the vDSO answers that from a shared page, without a syscall, whatever
# the clock source (RECORD.md, vDSO gap).
use strict; use warnings;
use IO::Socket::INET; use POSIX ();
# only modules of perl-base: no Time::HiRes, no Digest::MD5
sub nap { select(undef, undef, undef, $_[0]) }
sub md5_hex { sprintf '%08x', unpack('%32L*', $_[0] . "\0" x (4 - length($_[0]) % 4)) }
sub uptime { open(my $u, '<', '/proc/uptime'); (split ' ', <$u>)[0] }
$| = 1;
open(my $rnd, '<:raw', '/dev/urandom') or die;
my $srv = IO::Socket::INET->new(Listen => 5, LocalAddr => '127.0.0.1', LocalPort => 7900, ReuseAddr => 1) or die "listen: $!";
nap 1;
for (my $i = 0; ; $i++) {
    sysread($rnd, my $r, 24);
    my @n = unpack('L6', $r);
    # a child that computes and answers through a pipe
    pipe(my $rd, my $wr);
    my $pid = fork();
    if (!$pid) { close $rd; print $wr join(',', sort { $a <=> $b } @n); close $wr; POSIX::_exit($n[0] % 7); }
    close $wr; my $sorted = <$rd>; close $rd; waitpid($pid, 0); my $st = $? >> 8;
    # a file written, listed and read back
    my $f = "/scratch/perl.$i"; open(my $o, '>', $f) or die; print $o unpack('H*', $r), "\n" x ($n[1] % 5 + 1); close $o;
    my @st = stat($f); my @ls = sort grep { /^perl\./ } do { opendir(my $d, "/scratch"); readdir($d) };
    open(my $in, '<', $f); my @lines = <$in>; close $in; unlink $f;
    # a connection to ourselves
    my $c = IO::Socket::INET->new(PeerAddr => '127.0.0.1', PeerPort => 7900) or die "connect: $!";
    my $s = $srv->accept; print $c md5_hex($r), "\n"; $c->flush;
    my $got = <$s>; chomp $got; print $s scalar(reverse($got)), "\n"; $s->flush; my $back = <$c>; chomp $back;
    my $port = $c->sockport; close $c; close $s;
    printf "round %d up %s child %d:%s size %d lines %d ls %d port %d echo %s\n",
        $i, uptime(), $st, md5_hex($sorted), $st[7], scalar(@lines), scalar(@ls), $port, substr($back, 0, 12);
    nap 0.05;
}
