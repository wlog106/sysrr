#!/usr/bin/perl
# HTTP client for tests/test_httpd.sh (runs inside the container).
#   httpd_client.pl C N    C clients in parallel, N requests each over one
#                          keep-alive connection; every URL is different
use strict; use warnings; use IO::Socket::INET;
my ($c, $n) = @ARGV;
for my $id (1 .. $c) {
    next if fork();
    my $s = IO::Socket::INET->new(PeerAddr => '127.0.0.1', PeerPort => 80) or die "connect: $!";
    for my $i (1 .. $n) {
        my $path = $i % 5 ? "/?c=$id&i=$i" : "/missing-$id-$i";
        print $s "GET $path HTTP/1.1\r\nHost: t\r\n" . ($i == $n ? "Connection: close\r\n" : "") . "\r\n";
        my $len = 0;
        while (my $l = <$s>) { $len = $1 if $l =~ /^Content-Length: (\d+)/i; last if $l eq "\r\n" }
        read($s, my $body, $len) if $len;
        select(undef, undef, undef, 0.001) if $i % 4 == 0;
    }
    exit 0;
}
1 while wait() > 0;
