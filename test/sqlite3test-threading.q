#!/usr/bin/env qore

# sqlite3 test script

%modern


our o;

const opts =
    ( "help"    : "h,help",
      "db"      : "d,db=s",
      "enc"     : "e,encoding=s",
      "v"       : "v,verbose"
 );

sub usage() {
    printf("usage: %s [options]
 -h,--help          this help text.
 -v,--verbose       more verbose output.
 -d,--db=ARG        set database name - to create sqlite3 file name.
                    No \":memory:\" database is allowed in this case.
                    Warning: the database will not be cleaned up.
 -e,--encoding=ARG  set database character set encoding (i.e. \"utf8\")\n",
       basename(ENV."_"));
    exit();
}

auto g = new GetOpt(opts);
o = g.parse(\ARGV);

if (o.help) {
    usage();
}

if (!strlen(o.db)) {
    stderr.printf("set the DB file with -d, etc (-h for help)\n");
    exit(1);
}
if (elements ARGV) {
    stderr.printf("excess arguments on command-line (%n): -h for help\n", ARGV);
    exit(1);
}

if (o.db == ":memory:") {
    stderr.printf("This test cannot be run against the ':memory:' database. Use real DB file\n");
    exit(1);
}


# our ds = new Datasource("sqlite3", "", "", "test.db");
our ds = new DatasourcePool("sqlite3", "", "", o.db, o.enc, "", 5, 20);
our threadCount = 100;

string createStmt = "
    CREATE TABLE table1 (
        id integer primary key autoincrement,
        col1 text,
        col2 number,
        col3 text
    )
    ";

our insertStmt = "insert into table1 (col1, col2, col3)
                    values (%v, %v, %v)";

our selectStmt = "select * from table1 where id < %v";

our insertLock = new RWLock();


sub cout(string msg) {
    if (!o.v) {
        return;
    }
    printf("TID %d : %s\n", gettid(), msg);
}


sub insertWorker() {
    cout(sprintf("Starting insertWorker. Instances %d", threadCount));
    for (int i = 0; i < threadCount; ++i) {
        background insertExec();
    }
    cout("End insertWorker");
}

sub insertExec() {
    cout("Starting insertExec");

    insertLock.writeLock();
    on_exit insertLock.writeUnlock();

    ds.beginTransaction();
    try {
        int result = ds.exec(insertStmt, rand(), "Lorem Ipsum", rand());
        cout(sprintf("Inserted count: %d", result));
        ds.commit();
    } catch (hash<ExceptionInfo> ex) {
        stderr.printf("Insert failed (rollback will be performed): %N\n", ex);
        ds.rollback();
        rethrow;
    }

    cout("End insertExec");
}


sub selectWorker() {
    cout(sprintf("Starting selectWorker. Instances %d", threadCount));
    for (int i = 0; i < threadCount; ++i) {
        background selectExec();
    }
    cout("End selectWorker");
}

sub selectExec() {
    cout("Starting selectExec");
    auto result = ds.select(selectStmt, (rand() % 1000)); #threadCount+1);
    cout(sprintf("Selected count: %d", elements result."id"));
    cout("End selectExec");
}


try {
    ds.exec(createStmt);
    cout("table created");
} catch (hash<ExceptionInfo> ex) {
    stderr.printf("table is not created: %N\n", ex);
    stderr.printf("maybe it exists from previous run.");
    rethrow;
}

ds.commit();

background insertWorker();
usleep(300ms);
background selectWorker();
usleep(300ms);
background selectWorker();

ds.commit();
printf("All tests passed\n");
