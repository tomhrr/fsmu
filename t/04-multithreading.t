#!/usr/bin/perl

use warnings;
use strict;

use lib './t/lib';
use FsmuUtils qw(make_root_maildir
                 make_message
                 write_message
                 mu_init
                 mu_cmd);
use Digest::MD5;
use File::Basename;
use File::Find;
use File::Slurp qw(read_file);
use File::Spec::Functions qw(no_upwards);
use File::Temp qw(tempdir);
use List::Util qw(first);

use Test::More tests => 34;

my $mount_dir;
my $top_pid = $$;
my $pid;
my @pids;

sub debug
{
    my (@data) = @_;

    if ($ENV{'FSMU_DEBUG'}) {
        for my $d (@data) {
            print STDERR "$$: $d\n";
        }
    }

    return 1;
}

{
    my $dir = make_root_maildir();
    my $muhome = mu_init($dir);
    my $backing_dir = tempdir(UNLINK => 1);
    $mount_dir = tempdir(UNLINK => 1);
    my $refresh_cmd = "mu index --muhome=$muhome >/dev/null";
    if ($pid = fork()) {
        sleep(1);
    } else {
        my $res = system("./fsmu --muhome=$muhome ".
                         "--delete-remove --backing-dir=$backing_dir ".
                         "$mount_dir");
        sleep(3600);
        exit();
    }

    my @queries = (
        'from:user@example.org',
        'maildir:+asdf+asdf2',
        'maildir:+asdf+asdf3',
        'maildir:+asdf+asdf4',
        'to:asdf2@example.net',
        'to:asdf3@example.net',
        'to:asdf4@example.net',
        'data',
    );
    my @dirs = qw(cur new);

    my $get_existing_query_dir = sub {
        my $tries = 100;
        my $query;
        my $query_dir;
        do {
            $query = $queries[int(rand(@queries))];
            $query_dir = "$mount_dir/$query";
        } while ((not -e $query_dir) and ($tries--));
        return ($query, $query_dir);
    };

    my $get_existing_file = sub {
        my $tries = 100;
        my $query;
        my $query_dir;
        do {
            $query = $queries[int(rand(@queries))];
            $query_dir = "$mount_dir/$query";
        } while ((not -e $query_dir) and ($tries--));
        my $file;
        eval {
            my $dh;
            my $dir = $dirs[int(rand(@dirs))];
            opendir $dh, $query_dir.'/'.$dir or die $!;
            my @files = no_upwards readdir($dh);
            $file = $dir.'/'.$files[int(rand(@files))];
        };
        if (my $error = $@) {
            warn $error;
            return;
        }
        return "$query_dir/$file";
    };

    my %operations = (
        'query' => sub {
            my $query = $queries[int(rand(@queries))];
            my $query_dir = "$mount_dir/$query";
            if (-e $query_dir) {
                debug("$query exists, skipping");
            }
            my $res = mkdir($query_dir);
            if ($res) {
                debug("$query directory created");
            } else {
                debug("$query directory not created: $!");
            }
        },
        'remove-query' => sub {
            my $query = $queries[int(rand(@queries))];
            my $query_dir = "$mount_dir/$query";
            if (not -e $query_dir) {
                debug("$query does not exist, skipping");
            }
            my $res = rmdir($query_dir);
            if ($res) {
                debug("$query directory created");
            } else {
                debug("$query directory not created: $!");
            }
        },
        'refresh' => sub {
            my ($query, $query_dir) =
                $get_existing_query_dir->();
            if (not $query) {
                debug("no existing query directory found");
                return;
            }
            eval { read_file("$query_dir/.refresh") };
            if (my $error = $@) {
                debug("$query refresh failed: $error");
            } else {
                debug("$query refresh succeeded");
            }
        },
        'read-query' => sub {
            my ($query, $query_dir) =
                $get_existing_query_dir->();
            if (not $query) {
                debug("no existing query directory found");
                return;
            }
            eval {
                my $dh;
                opendir $dh, $query_dir or die $!;
                my @files = readdir($dh);
                closedir $dh or die $!;
            };
            if (my $error = $@) {
                debug("$query read failed: $error");
            } else {
                debug("$query read succeeded");
            }
        },
        'move' => sub {
            my $file = $get_existing_file->();
            if (not $file) {
                debug("move failed: can't get file");
                return;
            }
            my ($query, $query_dir) =
                $get_existing_query_dir->();
            if (not $query) {
                debug("move failed: no existing query directory found");
                return;
            }
            my $target = $file;
            my $v = int(rand(2));
            if ($v) {
                if ($target =~ /\/cur\//) {
                    $target =~ s/\/cur\//\/new\//;
                } else {
                    $target =~ s/\/new\//\/cur\//;
                }
            }
            $v = int(rand(2));
            if ($v) {
                my $basename = basename($target);
                if ($basename !~ /:/) {
                    $target .= ":2,S";
                }
            }
            my $res = rename($file, $target);
            if (not $res) {
                debug("move failed ($file, $target): $!");
            } else {
                debug("move succeeded");
            }
        },
        'read' => sub {
            my $file = $get_existing_file->();
            if (not $file) {
                debug("read failed: can't get file");
                return;
            }
            eval { read_file($file) };
            if (my $error = $@) {
                debug("read failed: $error");
            } else {
                debug("read succeeded");
            }
        },
        'delete' => sub {
            my $file = $get_existing_file->();
            if (not $file) {
                debug("delete failed: can't get file");
                return;
            }
            my $res = unlink $file;
            if (not $res) {
                debug("delete failed: $!");
            } else {
                debug("delete succeeded");
            }
        },
    );
    my @operation_names = keys %operations;

    my @pids;
    for (my $i = 0; $i < 8; $i++) {
        if (my $pid = fork()) {
            push @pids, $pid;
        } else {
            for (1..1000) {
                my $operation_name =
                    $operation_names[int(rand(@operation_names))];
                debug("$operation_name begin");
                $operations{$operation_name}->();
                debug("$operation_name end");
            }
            exit();
        }
    }

    for my $pid (@pids) {
        waitpid($pid, 0);
    }
}

END {
    if ($$ != $top_pid) {
        exit(0);
    }
    if ($mount_dir) {
        system("fusermount -u $mount_dir");
    }
    if ($pid) {
        kill('TERM', $pid);
        waitpid($pid, 0);
    }
    for my $pid (@pids) {
        kill('TERM', $pid);
        waitpid($pid, 0);
    }

    exit(0);
}

1;
