execbase=4
openlib=-408
open=-30
close=-36
write=-48

start:
	move.l	execbase,a6
	lea	dosname,a1
	moveq	#0,d0
	jsr	openlib(a6)
	move.l	d0,dosbase
	beq	error

	move.l	#nomfenetre,d1		;parametres fenetre
	move.l	#1005,d2		;mode ancien
	bsr	do_open
	beq	error
	move.l	d0,handle		;saugevarde du handle

	move.l	dosbase,a6
	move.l	handle,d1
	move.l	#titre,d2
	move.l	#fintitre-titre,d3
	jsr	write(a6)

	move.l	dosbase,a6
	move.l	handle,d1
	move.l	$fe0dc6,d2
	move.l	$fe0e13-fe0dc6,d3
	jsr	write(a6)
mouse:	
	btst	#6,$bfe001	
	bne.s   mouse
				
	move.l	handle,d1		;fermeture
	move.l	dosbase,a6
	jsr	close(a6)
	rts

error:
	rts
do_open:				;ouverture fenetre
	move.l	dosbase,a6
	jsr	open(a6)
	tst.l	d0
	rts
dosname:
	dc.b	'dos.library',0
	even
dosbase:
	blk.l	1
nomfenetre:
	dc.b	'CON:160/50/300/100/Message pour NEXUS',0
	even
handle:	
	blk.l	1
titre:
	dc.b	"By Amiguy in 1989                               "
	dc.b	"                                                "
	dc.b 	"Here is one of the secrets messages of the amiga"
	dc.b	"$FE0DC6-$FE0E13                                 "
fintitre:
even
