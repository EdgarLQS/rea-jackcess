package io.github.spannm.jackcess.impl;

import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Index;
import io.github.spannm.jackcess.Row;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.util.Random;

/**
 * Manual Jackcess 5.2.0 reference probe for index page splits and reopen ordering.
 * Run only on a disposable BIG_INDEX database copy.
 */
public final class JackcessIndexSplitProbe {
    private static final String EXTRA_TEXT = " some random text to fill out the index and make it fill up pages with"
        + " lots of extra bytes so that a few hundred rows are enough to give the index a root page with children under it";
    private static final int ROW_COUNT = 1_000;

    private JackcessIndexSplitProbe() {
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessIndexSplitProbe <writable-database-copy>");
        }

        File file = new File(args[0]);
        addRows(file);
        inspectAfterReopen(file);
    }

    private static void addRows(File file) throws Exception {
        try (Database database = openReadWrite(file)) {
            if (database.getFileFormat() != Database.FileFormat.V2000) {
                throw new IllegalArgumentException("Probe requires a Jet4 V2000 BIG_INDEX sample copy");
            }
            TableImpl table = (TableImpl) database.getTable("Table1");
            IndexImpl index = table.getIndex("col1");
            if (table.getRowCount() != 0 || index.getIndexData().getEntryCount() != 0) {
                throw new IllegalArgumentException("Probe requires a fresh BIG_INDEX sample copy");
            }

            Random random = new Random(13L);
            for (int i = 0; i < ROW_COUNT; i++) {
                int value = random.nextInt(Integer.MAX_VALUE);
                table.addRow(value + EXTRA_TEXT, "this is some row data");
            }
            index.getIndexData().validate(false);
            database.flush();
        }
    }

    private static void inspectAfterReopen(File file) throws Exception {
        try (Database database = openReadOnly(file)) {
            TableImpl table = (TableImpl) database.getTable("Table1");
            IndexImpl index = table.getIndex("col1");
            IndexData indexData = index.getIndexData();
            indexData.validate(false);

            DatabaseImpl databaseImpl = (DatabaseImpl) database;
            JetFormat format = databaseImpl.getFormat();
            ByteBuffer page = databaseImpl.getPageChannel().createPageBuffer();
            int rootPageNumber = indexData.getRootPageNumber();
            databaseImpl.getPageChannel().readPage(page, rootPageNumber);
            if (page.get(0) != PageTypes.INDEX_NODE) {
                throw new IllegalStateException("Root page is not an index node after page splits");
            }
            int rootLevel = getLevel(page, format);
            if (rootLevel < 1) {
                throw new IllegalStateException("Index root level did not rise above the leaf level");
            }

            int leafPages = 0;
            int nodePages = 0;
            long databaseSize = Files.size(file.toPath());
            if (databaseSize % format.PAGE_SIZE != 0) {
                throw new IOException("Database size is not a whole number of pages: " + databaseSize);
            }
            int pageCount = Math.toIntExact(databaseSize / format.PAGE_SIZE);
            for (int pageNumber = 1; pageNumber < pageCount; pageNumber++) {
                databaseImpl.getPageChannel().readPage(page, pageNumber);
                byte pageType = page.get(0);
                int level = getLevel(page, format);
                if (pageType == PageTypes.INDEX_LEAF) {
                    if (level != 0) {
                        throw new IllegalStateException("Index leaf page has a nonzero level: " + pageNumber);
                    }
                    leafPages++;
                } else if (pageType == PageTypes.INDEX_NODE) {
                    if (level < 1) {
                        throw new IllegalStateException("Index node page has a zero level: " + pageNumber);
                    }
                    nodePages++;
                }
            }
            if (leafPages == 0 || nodePages == 0) {
                throw new IllegalStateException("Expected both leaf and node pages after index splits");
            }

            int rowCount = 0;
            String previous = null;
            String first = null;
            String last = null;
            for (Row row : CursorBuilder.createCursor(index)) {
                String value = (String) row.get("col1");
                if (previous != null && previous.compareTo(value) > 0) {
                    throw new IllegalStateException("Index cursor is not sorted at entry " + rowCount);
                }
                if (first == null) {
                    first = value;
                }
                previous = value;
                last = value;
                rowCount++;
            }
            if (rowCount != ROW_COUNT) {
                throw new IllegalStateException("Index cursor returned " + rowCount + " rows; expected " + ROW_COUNT);
            }

            System.out.println("format=" + database.getFileFormat());
            System.out.println("rows_after_reopen=" + rowCount);
            System.out.println("index_entries_after_reopen=" + indexData.getEntryCount());
            System.out.println("root_page_number=" + rootPageNumber);
            System.out.println("root_level=" + rootLevel);
            System.out.println("index_leaf_pages=" + leafPages);
            System.out.println("index_node_pages=" + nodePages);
            System.out.println("index_sorted=true");
            System.out.println("first_key=" + first);
            System.out.println("last_key=" + last);
        }
    }

    private static Database openReadOnly(File file) throws Exception {
        return new DatabaseBuilder().withFile(file).withReadOnly(true).open();
    }

    private static Database openReadWrite(File file) throws Exception {
        return new DatabaseBuilder().withFile(file).withReadOnly(false).open();
    }

    private static int getLevel(ByteBuffer page, JetFormat format) {
        return ByteUtil.getUnsignedByte(page, format.OFFSET_INDEX_LEVEL);
    }
}
