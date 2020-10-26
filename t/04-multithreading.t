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

use Test::More tests => 13;

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
    my ($muhome, $refresh_cmd) = mu_init($dir);
    my $backing_dir = tempdir(UNLINK => 1);
    $mount_dir = tempdir(UNLINK => 1);
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
    my @addresses = (
        'user@example.org',
        'asdf1@example.net',
        'asdf2@example.net',
        'asdf3@example.net',
        'asdf4@example.net',
        'asdf5@example.net',
    );

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

    my @root_maildirs = qw(asdf qwer zxcv tyui ghjk);
    my @sub_maildirs = qw(asdf1 asdf2 asdf3 asdf4 asdf5);
    my $get_existing_maildir = sub {
        return $dir.'/'.
               $root_maildirs[int(rand(@root_maildirs))].'/'.
               $sub_maildirs[int(rand(@sub_maildirs))];
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
            no warnings;
            opendir $dh, $query_dir.'/'.$dir or die $!;
            my @files = no_upwards readdir($dh);
            $file = $dir.'/'.$files[int(rand(@files))];
        };
        if (my $error = $@) {
            if ($error !~ /No such file or directory/) {
                warn $error;
            }
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
                return;
            }
            my $res = mkdir($query_dir);
            if ($res) {
                debug("$query directory created");
            } else {
                debug("$query directory not created: $!");
            }
            find (sub {}, $query_dir);
        },
        'remove-query' => sub {
            my $query = $queries[int(rand(@queries))];
            my $query_dir = "$mount_dir/$query";
            if (not -e $query_dir) {
                debug("$query does not exist, skipping");
                return;
            }
            my $res = rmdir($query_dir);
            if ($res) {
                debug("$query directory removed");
            } else {
                debug("$query directory not removed: $!");
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
        'write-message' => sub {
            my $entity = make_message($addresses[int(rand(@addresses))],
                                      $addresses[int(rand(@addresses))],
                                      'new-subject',
                                      'new-data');
            my $maildir = $get_existing_maildir->();
            if (not $maildir) {
                debug("write-message failed: no existing maildir ".
                      "directory found");
                return;
            }
            eval {
                write_message($entity, $maildir.'/'.$dirs[int(rand(@dirs))]);
            };
            if (my $error = $@) {
                debug("write-message failed: $error");
            } else {
                debug("write-message succeeded");
            }
        },
    );
    my @operation_names = keys %operations;

    for my $query (@queries) {
        my $query_dir = "$mount_dir/$query";
        my $res = mkdir($query_dir);
        ok($res, "Made query directory for '$query'");
        find (sub {}, $query_dir);
    }

    my @pids;
    for (my $i = 0; $i < 8; $i++) {
        if (my $pid = fork()) {
            push @pids, $pid;
        } else {
            for my $i (1..1000) {
                my $operation_name =
                    $operation_names[int(rand(@operation_names))];
                if (($operation_name eq 'remove-query')
                        and ($i % 50 != 0)) {
                    next;
                }
                if (($operation_name eq 'delete')
                        and ($i % 100 != 0)) {
                    next;
                }
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

    sleep(2);
    ok(1, 'Operations completed');

    # As a proxy for confirming that the operations were all handled
    # correctly (regardless of whether they succeeded or failed),
    # confirm that the set of reverse paths is correct.
    my @paths;
    find(sub {
        if ((-f $File::Find::name)
                and ($File::Find::name !~ /\.last-update/)) {
            push @paths, $File::Find::name;
        }
    }, $mount_dir);
    my $scalar_paths = scalar @paths;
    ok(@paths, "Found some files ($scalar_paths) in the mount directory");

    my %path_lookup =
        map { my $path = $_;
              $path =~ s/$mount_dir\///;
              $path => 1 }
            @paths;

    my @reverse_paths;
    find(sub {
        if (-f $File::Find::name) {
            push @reverse_paths, $File::Find::name;
        }
    }, $backing_dir.'/_reverse');

    my $exists = 0;
    my $not_exists = 0;
    for my $reverse_path (@reverse_paths) {
        my $real_path = readlink($reverse_path);
        $real_path =~ s/$backing_dir\/_//;
        if ($path_lookup{$real_path}) {
            $exists++;
            delete $path_lookup{$real_path};
        } else {
            $not_exists++;
        }
    }

    ok($exists, 'Found some reverse path mappings');
    ok((not $not_exists), 'All reverse paths map to real files');
    my $res = ok((not keys %path_lookup),
        'All real files map to reverse paths');
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
