#!/usr/bin/perl

use warnings;
use strict;

use lib './t/lib';
use FsmuUtils qw(make_root_maildir
                 mu_init
                 mu_cmd);
use autodie;
use File::Find;
use File::Temp qw(tempdir);
use Proc::ProcessTable;

use Test::More tests => 2;

my $mount_dir;
my $pid;

{
    my @help = `./fsmu --help`;
    like($help[0], qr/^usage/, 'Got help details');

    my $dir = make_root_maildir(10, 10);
    my ($muhome, $refresh_cmd) = mu_init($dir);
    my $backing_dir = tempdir(UNLINK => 1);
    $mount_dir = tempdir(UNLINK => 1);
    if ($pid = fork()) {
        sleep(1);
    } else {
        my $res = system("./fsmu -s --muhome=$muhome ".
                         "--delete-remove --backing-dir=$backing_dir ".
                         "$mount_dir");
        sleep(3600);
        exit();
    }

    my $initial_memory;
    my $p = Proc::ProcessTable->new(cache_ttys => 1);
    for my $proc (@{$p->table()}) {
        if ($proc->cmndline() =~ /fsmu.*$muhome/) {
            $initial_memory = $proc->rss();
        }
    }
    if (not $initial_memory) {
        die "Unable to find fsmu process";
    }

    # Query for all of the messages, and then print out the amount of
    # memory required by the fsmu process.

    my $query_dir = $mount_dir.'/data';
    mkdir $query_dir;
    my @query_files;
    find(sub { push @query_files, $File::Find::name },
         $query_dir);
    is(@query_files, 503, '503 mail items found');

    my $post_query_memory;
    $p = Proc::ProcessTable->new(cache_ttys => 1);
    for my $proc (@{$p->table()}) {
        if ($proc->cmndline() =~ /fsmu/) {
            $post_query_memory = $proc->rss();
        }
    }
    if (not $post_query_memory) {
        die "Unable to find fsmu process";
    }

    my $diff = $post_query_memory - $initial_memory;
    $diff /= 1000000;
    diag "${diff}M memory required for search\n";
}

END {
    if ($mount_dir) {
        system("fusermount -u $mount_dir");
    }
    if ($pid) {
        kill('TERM', $pid);
        waitpid($pid, 0);
    }
    exit(0);
}

1;
