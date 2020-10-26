## fsmu

[![Build Status](https://travis-ci.org/tomhrr/dale.svg?branch=master)](https://travis-ci.org/tomhrr/dale)

A virtual maildir filesystem for [`mu`](https://github.com/djcb/mu)
queries.  Create directories with names that are `mu` queries, and
those directories will be populated with the query results on
retrieval.  Movement of mail within the search directory (e.g. from
`new` to `cur`) propagates to the underlying maildir, too.

### Dependencies

  * [FUSE](https://github.com/libfuse/libfuse)
  * [`mu`](https://github.com/djcb/mu) (>= 1.2.0)

### Install

    make
    sudo make install

### Usage

Create a backing directory:

    $ mkdir fsmu-bd

Mount the filesystem:

    $ fsmu --backing-dir=fsmu-bd fsmu

Create and retrieve a query directory:

    $ cd fsmu
    $ mkdir from:user@example.net
    $ ls from:user@example.net
    cur new tmp
    $ mutt -f from:user@example.net
    ...

### Behaviour

`mkdir` can be used at the top-level to create query directories.  Any
`+` character in the directory name will be converted into a `/`
before being used as a query, to work around `/` not being permitted
in file/directory names.  `rmdir` can be used to remove a query
directory, regardless of whether it has been populated.

Movement of mail within a query directory is supported, and propagates
to the underlying maildir, as well as to any other query directories
that have the same message.  This means that changes to a message's
flags, as well as movement from `new` to `cur` and vice-versa, take
effect on the message in those other locations.  If the movement is
such that the only change to the filename is to its maildir flags,
then propagation applies that change to the original filename in the
maildir and any other affected query directories, rather than using
the target filename from the first movement operation.

Whenever `cur` or `new` within the query directory is accessed, the
query results are refreshed if that hasn't happened within the last 30
seconds.  This value can be changed by way of the `--refresh-timeout`
option.  A query directory can be forcibly refreshed by attempting to
read a file named `.refresh` at the top-level of the query directory.

By default, deletion is not supported.  To have deletion take effect
in both the query directory and the underlying maildir, pass the
`--delete-remove` option.

If a directory name at the top-level is not a valid query, then
attempting to access that directory will lead to an "operation not
permitted" error message.

The path to the `mu` executable can be set by using the `--mu` option,
and the `muhome` configuration option (passed to the `mu` commands)
can be set by using the `--muhome` option.

Debug and error information is logged using syslog.

### Bugs/problems/suggestions

See the [GitHub issue tracker](https://github.com/tomhrr/fsmu/issues).

### Acknowledgments

 * Most of the general approach/interface here follows [notmuchfs](https://github.com/tsto/notmuchfs).

### Licence

See LICENCE.
