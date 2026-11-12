#!/usr/bin/perl
# memcached client for tests/test_memcached.sh (runs inside the container).
#   mc_client.pl load N        N keys, one connection
#   mc_client.pl work C OPS    C clients in parallel, OPS operations each, every
#                              client on keys of its own (the server's threads
#                              still share its tables, locks and memory)
#   mc_client.pl race C OPS    the same, all clients on the same keys: which
#                              of two requests came first is then a race
#   mc_client.pl dump          every key the two commands above can touch, in order
use strict; use warnings; use IO::Socket::INET;
sub conn { IO::Socket::INET->new(PeerAddr => '127.0.0.1', PeerPort => 11211) or die "connect: $!" }
sub cmd { my ($s, $c) = @_; print $s $c; my $r = ''; while (my $l = <$s>) { $r .= $l; last if $l =~ /^(END|STORED|NOT_STORED|NOT_FOUND|DELETED|ERROR|\d+)\r/ } $r }
my $mode = shift // 'dump';
if ($mode eq 'load') {
    my $s = conn();
    cmd($s, "set pre:$_ 0 0 " . length("v$_") . "\r\nv$_\r\n") for 1 .. shift;
} elsif ($mode eq 'work' || $mode eq 'race') {
    my ($c, $ops) = @ARGV;
    for my $id (1 .. $c) {
        next if fork();
        srand($id); my $s = conn();
        for my $i (1 .. $ops) {
            my $own = $mode eq 'work' ? "c$id." : '';
            my $k = "${own}k" . int(rand($own ? 20 : 150)); my $r = int(rand(5));
            if ($r == 0)    { my $v = "c$id-$i" x (1 + int(rand(40))); cmd($s, "set $k 0 0 " . length($v) . "\r\n$v\r\n") }
            elsif ($r == 1) { cmd($s, "get $k\r\n") }
            elsif ($r == 2) { cmd($s, "append $k 0 0 " . length("+$id") . "\r\n+$id\r\n") }
            elsif ($r == 3) { my $n = "${own}n" . int(rand($own ? 3 : 20)); cmd($s, "incr $n $id\r\n") =~ /NOT_FOUND/ and cmd($s, "add $n 0 0 1\r\n0\r\n") }
            else            { cmd($s, "delete pre:" . ($own ? ($id - 1) * 12 + 1 + int(rand(12)) : int(rand(100))) . "\r\n") }
        }
        exit 0;
    }
    1 while wait() > 0;
} else {
    my $s = conn();
    my @own = map { my $c = $_; (map("c$c.k$_", 0 .. 19), map("c$c.n$_", 0 .. 2)) } 1 .. 8;
    print cmd($s, "get $_\r\n") for (map("pre:$_", 1 .. 100), map("k$_", 0 .. 149), map("n$_", 0 .. 19), @own);
}
