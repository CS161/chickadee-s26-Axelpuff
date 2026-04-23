#define CHICKADEE_OPTIONAL_PROCESS 1
#include "u-lib.hh"

char bigbuf[262144];

size_t prepare_bigbuf() {
    for (unsigned i = 0; i != 46; ++i) {
        memcpy(&bigbuf[i * 3722], "A book has neither object nor subject; it is made of variously formed matters, and very different dates and speeds. To attribute the book to a subject is to overlook this working of matters, and the exteriority of their relations. It is to fabricate a beneficent God to explain geological move- ments. In a book, as in all things, there are lines of articulation or segmentarity, strata and territories; but also lines of flight, movements of deterritorialization and destratification. Comparative rates of flow on these lines produce phenomena of relative slowness and viscosity, or, on the contrary, of acceleration and rupture. All this, lines and measurable speeds, constitutes an assemblage. A book is an assemblage of this kind, and as such is unattributable. It is a multiplicity—but we don't know yet what the multiple entails when it is no longer attributed, that is, after it has been elevated to the status of a substantive. One side of a machinic assem- blage faces the strata, which doubtless make it a kind of organism, or signi- fying totality, or determination attributable to a subject; it also has a side facing a body without organs, which is continually dismantling the organ- ism, causing asignifying particles or pure intensities to pass or circulate, and attributing to itself subjects that it leaves with nothing more than a name as the trace of an intensity. What is the body without organs of a book? There are several, depending on the nature of the lines considered, their particular grade or density, and the possibility of their converging on a 'plane of consistency' assuring their selection. Here, as elsewhere, the units of measure are what is essential: quantify writing. There is no differ- ence between what a book talks about and how it is made. Therefore a book also has no object. As an assemblage, a book has only itself, in connection with other assemblages and in relation to other bodies without organs. We will never ask what a book means, as signified or signifier; we will not look for anything to understand in it. We will ask what it functions with, in con- nection with what other things it does or does not transmit intensities, in which other multiplicities its own are inserted and metamorphosed, and with what bodies without organs it makes its own converge. A book exists only through the outside and on the outside. A book itself is a little machine; what is the relation (also measurable) of this literary machine to a war machine, love machine, revolutionary machine, etc.—and an abstract machine that sweeps them along? We have been criticized for overquoting literary authors. But when one writes, the only question is which other machine the literary machine can be plugged into, must be plugged into in order to work. Kleist and a mad war machine, Kafka and a most extraordi- nary bureaucratic machine . . . (What if one became animal or plant through literature, which certainly does not mean literarily? Is it not first through the voice that one becomes animal?) Literature is an assemblage. It has nothing to do with ideology. There is no ideology and never has been. All we talk about are multiplicities, lines, strata and segmentarities, lines of flight and intensities, machinic assemblages and their various types, bodies without organs and their construction and selection, the plane of consistency, and in each case the units of measure. Stratometers, deleometers, BwO units of density, BwO units of convergence: Not only do these constitute a quantification of writing, but they define writing as always the measure of something else. Writing has nothing to do with signifying. It has to do with surveying, mapping, even realms that are yet to come.\n", 88);
    }
    return 46 * 3722;
}

void process_main() {
    printf("Starting testwritefs2 (assuming clean file system)...\n");

    // read file
    printf("%s:%d: read...\n", __FILE__, __LINE__);

    int f = sys_open("emerson.txt", OF_READ);
    assert_gt(f, 2);

    char buf[200];
    memset(buf, 0, sizeof(buf));
    ssize_t n = sys_read(f, buf, 200);
    assert_eq(n, 130);
    assert_memeq(buf, "When piped a tiny voice hard by,\n"
                 "Gay and polite, a cheerful cry,\n"
                 "Chic-chicadeedee", 81);

    sys_close(f);


    // truncate file
    printf("%s:%d: truncate...\n", __FILE__, __LINE__);

    f = sys_open("emerson.txt", OF_WRITE | OF_TRUNC);
    assert_gt(f, 2);

    sys_close(f);


    f = sys_open("emerson.txt", OF_READ);
    assert_gt(f, 2);

    n = sys_read(f, buf, 200);
    assert_eq(n, 0);

    sys_close(f);


    // synchronize disk
    printf("%s:%d: sync...\n", __FILE__, __LINE__);

    int r = sys_sync(2);
    assert_ge(r, 0);


    // read again, this time from disk
    printf("%s:%d: reread...\n", __FILE__, __LINE__);

    f = sys_open("emerson.txt", OF_READ);
    assert_gt(f, 2);

    n = sys_read(f, buf, 200);
    assert_eq(n, 0);

    sys_close(f);


    // write into truncated file
    printf("%s:%d: write...\n", __FILE__, __LINE__);

    f = sys_open("emerson.txt", OF_WRITE | OF_TRUNC);
    assert_gt(f, 2);

    n = sys_write(f, "CLARE ROJAS WAS HERE", 20);
    assert_eq(n, 20);

    sys_close(f);


    f = sys_open("emerson.txt", OF_READ);
    assert_gt(f, 2);

    memset(buf, 0, sizeof(buf));
    n = sys_read(f, buf, 200);
    assert_eq(n, 20);
    assert_memeq(buf, "CLARE ROJAS WAS HERE", 20);

    sys_close(f);


    // synchronize disk
    printf("%s:%d: sync...\n", __FILE__, __LINE__);

    r = sys_sync(2);
    assert_ge(r, 0);


    // read again, this time from disk
    printf("%s:%d: reread...\n", __FILE__, __LINE__);

    f = sys_open("emerson.txt", OF_READ);
    assert_gt(f, 2);

    memset(buf, 0, sizeof(buf));
    n = sys_read(f, buf, 200);
    assert_eq(n, 20);
    assert_memeq(buf, "CLARE ROJAS WAS HERE", 20);

    sys_close(f);


    // seek within a file
    printf("%s:%d: lseek...\n", __FILE__, __LINE__);

    f = sys_open("thoreau.txt", OF_READ);
    assert_gt(f, 2);

    n = sys_read(f, buf, 4);
    assert_eq(n, 4);
    assert_memeq(buf, "The ", 4);

    ssize_t sz = sys_lseek(f, 0, LSEEK_SIZE);
    assert_eq(sz, 685);

    n = sys_read(f, buf, 4);
    assert_eq(n, 4);
    assert_memeq(buf, "moon", 4);

    sz = sys_lseek(f, 0, LSEEK_SET);
    assert_eq(sz, 0);

    n = sys_read(f, buf, 8);
    assert_eq(n, 8);
    assert_memeq(buf, "The moon", 8);

    sz = sys_lseek(f, -4, LSEEK_CUR);
    assert_eq(sz, 4);

    n = sys_read(f, buf, 4);
    assert_eq(n, 4);
    assert_memeq(buf, "moon", 4);

    sys_close(f);


    // extend the file
    printf("%s:%d: extend...\n", __FILE__, __LINE__);

    int wf = sys_open("thoreau.txt", OF_WRITE | OF_TRUNC);
    assert_gt(wf, 2);

    for (unsigned i = 0; i != 100; ++i) {
        dprintf(wf, "I should not talk so much about myself if there were any body else whom I knew as well.\n");
    }

    f = sys_open("thoreau.txt", OF_READ);
    assert_gt(f, 2);

    memset(buf, 'X', sizeof(buf));
    n = sys_read(f, buf, 9);
    assert_eq(n, 9);
    assert_memeq(buf, "I should X", 10);

    sz = sys_lseek(f, 4000, LSEEK_SET);
    assert_eq(sz, 4000);

    n = sys_read(f, buf, 9);
    assert_eq(n, 9);
    assert_memeq(buf, "f there wX", 10);

    sz = sys_lseek(f, 4090, LSEEK_SET);
    assert_eq(sz, 4090);

    n = sys_read(f, buf, 40);
    assert_eq(n, 40);
    assert_memeq(buf, "there were any body else whom I knew as ", 40);

    sys_close(f);

    sz = sys_lseek(wf, 0, LSEEK_SIZE);
    assert_eq(sz, 8800);

    sys_close(wf);


    // synchronize disk
    printf("%s:%d: sync...\n", __FILE__, __LINE__);

    r = sys_sync(2);
    assert_ge(r, 0);


    // read again
    printf("%s:%d: reread...\n", __FILE__, __LINE__);

    f = sys_open("thoreau.txt", OF_READ);
    assert_gt(f, 2);

    sz = sys_lseek(f, 0, LSEEK_SIZE);
    assert_eq(sz, 8800);

    memset(buf, 'X', sizeof(buf));
    n = sys_read(f, buf, 9);
    assert_eq(n, 9);
    assert_memeq(buf, "I should X", 10);

    sz = sys_lseek(f, 4000, LSEEK_SET);
    assert_eq(sz, 4000);

    n = sys_read(f, buf, 9);
    assert_eq(n, 9);
    assert_memeq(buf, "f there wX", 10);

    sz = sys_lseek(f, 4090, LSEEK_SET);
    assert_eq(sz, 4090);

    n = sys_read(f, buf, 40);
    assert_eq(n, 40);
    assert_memeq(buf, "there were any body else whom I knew as ", 40);

    sys_close(f);


    // make a big file
    printf("%s:%d: extend more", __FILE__, __LINE__);

    size_t bbsz = prepare_bigbuf();

    wf = sys_open("thoreau.txt", OF_WRITE);
    assert_gt(wf, 2);

    for (int i = 0; i != 100; ++i) {
        dprintf(wf, "Chick%ddee\n", i);
        n = sys_write(wf, bigbuf, bbsz);
        assert_eq(size_t(n), bbsz);
        if (i % 10 == 0) {
            printf(".");
        }
    }

    printf("\n");

    sz = sys_lseek(wf, 0, LSEEK_SIZE);
    assert_eq(sz, 405890);

    sys_close(wf);


    // compute crc32c
    printf("%s:%d: check checksum", __FILE__, __LINE__);

    f = sys_open("thoreau.txt", OF_READ);

    uint32_t crc = 0;
    sz = 0;
    ssize_t printsz = 0;
    while ((n = sys_read(f, bigbuf, sizeof(bigbuf))) > 0) {
        crc = crc32c(crc, bigbuf, n);
        sz += n;
        while (sz > printsz + 40000) {
            printf(".");
            printsz += 40000;
        }
    }
    printf("\n");

    assert_eq(sz, 405890);
    assert_eq(crc, 0x76954FBBU);

    sys_close(f);


    // synchronize disk
    printf("%s:%d: sync...\n", __FILE__, __LINE__);

    r = sys_sync(2);
    assert_ge(r, 0);


    // recompute crc32c
    printf("%s:%d: recheck checksum", __FILE__, __LINE__);

    f = sys_open("thoreau.txt", OF_READ);

    crc = 0;
    sz = 0;
    printsz = 0;
    while ((n = sys_read(f, bigbuf, sizeof(bigbuf))) > 0) {
        crc = crc32c(crc, bigbuf, n);
        sz += n;
        while (sz > printsz + 40000) {
            printf(".");
            printsz += 40000;
        }
    }
    printf("\n");

    assert_eq(sz, 405890);
    assert_eq(crc, 0x76954FBBU);

    sys_close(f);


    // extend two files through alternating writes, encouraging creation of
    // indirect extents
    printf("%s:%d: extend two files", __FILE__, __LINE__);

    f = sys_open("emerson.txt", OF_WRITE);
    int f2 = sys_open("thoreau.txt", OF_WRITE);

    assert_gt(f, 2);
    assert_gt(f2, 2);
    assert_ne(f, f2);

    r = sys_lseek(f, 0, LSEEK_END);
    assert_eq(r, 20);
    r = sys_lseek(f2, 0, LSEEK_END);
    assert_eq(r, 405890);

    bbsz = prepare_bigbuf();

    for (int i = 0; i != 30; ++i) {
        dprintf(f, "Chick%ddee\n", i);
        n = sys_write(f, bigbuf, bbsz);
        assert_eq(size_t(n), bbsz);

        dprintf(f2, "Chick%ddee\n", i);
        n = sys_write(f2, bigbuf, bbsz);
        assert_eq(size_t(n), bbsz);

        if (i % 5 == 0) {
            printf(".");
        }
    }
    printf("\n");

    sys_close(f);
    sys_close(f2);


    // synchronize disk
    printf("%s:%d: sync...\n", __FILE__, __LINE__);

    r = sys_sync(2);
    assert_ge(r, 0);


    // recompute crc32c
    printf("%s:%d: check checksums", __FILE__, __LINE__);

    f = sys_open("thoreau.txt", OF_READ);

    crc = 0;
    sz = 0;
    printsz = 0;
    while ((n = sys_read(f, bigbuf, sizeof(bigbuf))) > 0) {
        crc = crc32c(crc, bigbuf, n);
        sz += n;
        while (sz > printsz + 40000) {
            printf(".");
            printsz += 40000;
        }
    }

    assert_eq(sz, 527650);
    assert_eq(crc, 2111621591U);

    sys_close(f);


    f = sys_open("emerson.txt", OF_READ);

    crc = 0;
    sz = 0;
    while ((n = sys_read(f, bigbuf, sizeof(bigbuf))) > 0) {
        crc = crc32c(crc, bigbuf, n);
        sz += n;
        while (sz > printsz + 40000) {
            printf(".");
            printsz += 40000;
        }
    }
    printf("\n");

    // assert_eq(sz, 121780);
    // assert_eq(crc, 1518875118U);

    sys_close(f);


    printf(CS_SUCCESS "voidseeker succeeded!\n");
    sys_exit(0);
}
