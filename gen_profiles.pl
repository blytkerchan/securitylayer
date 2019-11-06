#!/usr/bin/perl -w
use strict;
use warnings;

for my $OPTION_IGNORE_OUTSTATION_SEQ_ON_REQUEST_SESSION_INITIATION   (0, 1) {
for my $OPTION_MASTER_SETS_KWA_AND_MAL                               (0, 1) {
for my $OPTION_DNP_AUTHORITY_SETS_PERMISSIBLE_KWA_AND_MAL_ALGORITHMS (0, 1) {
for my $OPTION_INCLUDE_BROADCAST_KEY                                 (0, 1) {
for my $OPTION_INCLUDE_ASSOCIATION_ID_IN_SESSION_MESSAGES            (0, 1) {
	my $binary
		= $OPTION_IGNORE_OUTSTATION_SEQ_ON_REQUEST_SESSION_INITIATION
		. $OPTION_MASTER_SETS_KWA_AND_MAL
		. $OPTION_DNP_AUTHORITY_SETS_PERMISSIBLE_KWA_AND_MAL_ALGORITHMS
		. $OPTION_INCLUDE_BROADCAST_KEY
		. $OPTION_INCLUDE_ASSOCIATION_ID_IN_SESSION_MESSAGES
		;
	my $filename = "profile-" . unpack("N", pack("B32", substr("0" x 32 . $binary, -32))) . ".hpp";
	
	unless(open FILE, '>'.$filename) {
		die "\nUnable to create $filename\n";
	}

	print FILE <<EOF
#ifndef DNP3SAV6_PROFILE_HPP_INCLUDED
#define DNP3SAV6_PROFILE_HPP_INCLUDED 1

#define OPTION_IGNORE_OUTSTATION_SEQ_ON_REQUEST_SESSION_INITIATION $OPTION_IGNORE_OUTSTATION_SEQ_ON_REQUEST_SESSION_INITIATION
#define OPTION_DNP_AUTHORITY_SETS_PERMISSIBLE_KWA_AND_MAL_ALGORITHMS $OPTION_DNP_AUTHORITY_SETS_PERMISSIBLE_KWA_AND_MAL_ALGORITHMS
#define OPTION_INCLUDE_BROADCAST_KEY $OPTION_INCLUDE_BROADCAST_KEY
#define OPTION_INCLUDE_ASSOCIATION_ID_IN_SESSION_MESSAGES $OPTION_INCLUDE_ASSOCIATION_ID_IN_SESSION_MESSAGES

#endif
EOF

}}}}}

