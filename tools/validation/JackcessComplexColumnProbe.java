import io.github.spannm.jackcess.Column;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.complex.Attachment;
import io.github.spannm.jackcess.complex.ComplexColumnInfo;
import io.github.spannm.jackcess.complex.ComplexDataType;
import io.github.spannm.jackcess.complex.ComplexValueForeignKey;
import io.github.spannm.jackcess.complex.SingleValue;
import io.github.spannm.jackcess.complex.UnsupportedValue;
import io.github.spannm.jackcess.complex.Version;
import io.github.spannm.jackcess.impl.ColumnImpl;
import io.github.spannm.jackcess.impl.complex.ComplexColumnInfoImpl;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Manual Jackcess 5.2.0 read-only probe for ACE complex-column fixtures.
 */
public final class JackcessComplexColumnProbe {
    private JackcessComplexColumnProbe() {
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 0) {
            throw new IllegalArgumentException("Usage: JackcessComplexColumnProbe <complex-database-copy>...");
        }
        for (String arg : args) {
            inspect(new File(arg));
        }
    }

    private static void inspect(File file) throws Exception {
        try (Database database = new DatabaseBuilder().withFile(file).withReadOnly(true).open()) {
            boolean unsupportedFixture = file.getName().toLowerCase().contains("unsupported");
            String tableName = unsupportedFixture ? "Test" : "Table1";
            Table table = database.getTable(tableName);
            System.out.println("sample=" + file.getName());
            System.out.println("format=" + database.getFileFormat());
            System.out.println("table=" + table.getName() + " rows=" + table.getRowCount());

            if (unsupportedFixture) {
                inspectColumn(table, table.getColumn("UnknownComplex"), "ID");
            } else {
                int complexColumns = 0;
                for (Column column : table.getColumns()) {
                    if (column.getType() == DataType.COMPLEX_TYPE) {
                        inspectColumn(table, column, "id");
                        complexColumns++;
                    }
                }
                if (complexColumns != 3) {
                    throw new IllegalStateException("Expected three complex columns, found " + complexColumns);
                }
            }
            System.out.println("sample_inspection=passed");
        }
    }

    private static void inspectColumn(Table table, Column column, String rowIdColumn) throws Exception {
        ComplexColumnInfo<?> info = column.getComplexInfo();
        if (info == null) {
            throw new IllegalStateException("Complex-column metadata is missing for " + column.getName());
        }
        ComplexColumnInfoImpl<?> infoImpl = (ComplexColumnInfoImpl<?>) info;
        Column fkColumn = infoImpl.getComplexValueForeignKeyColumn();
        Column pkColumn = infoImpl.getPrimaryKeyColumn();
        if (!pkColumn.isAutoNumber() || !((ColumnImpl) fkColumn).isComplexValueForeignKey()) {
            throw new IllegalStateException("Unexpected complex-table key columns for " + column.getName());
        }

        int totalValues = 0;
        List<String> rowCounts = new ArrayList<>();
        for (Row row : table) {
            Object rowValue = column.getRowValue(row);
            if (!(rowValue instanceof ComplexValueForeignKey)) {
                throw new IllegalStateException("Complex value foreign key is missing in " + column.getName());
            }
            ComplexValueForeignKey foreignKey = (ComplexValueForeignKey) rowValue;
            int count = foreignKey.countValues();
            totalValues += count;
            rowCounts.add(row.get(rowIdColumn) + ":" + count);
            inspectValues(info.getType(), foreignKey, count);
        }

        System.out.println("column=" + column.getName()
            + " type=" + info.getType()
            + " flat_table=" + fkColumn.getTable().getName()
            + " pk=" + pkColumn.getName()
            + " fk=" + fkColumn.getName()
            + " fk_flag=" + ((ColumnImpl) fkColumn).isComplexValueForeignKey()
            + " values=" + totalValues
            + " per_row=" + rowCounts);
    }

    private static void inspectValues(ComplexDataType type, ComplexValueForeignKey foreignKey, int expectedCount)
        throws Exception {
        int actualCount;
        switch (type) {
        case ATTACHMENT:
            List<Attachment> attachments = foreignKey.getAttachments();
            actualCount = attachments.size();
            for (Attachment attachment : attachments) {
                attachment.getFileData();
                attachment.getEncodedFileData();
            }
            break;
        case MULTI_VALUE:
            List<SingleValue> multiValues = foreignKey.getMultiValues();
            actualCount = multiValues.size();
            for (SingleValue multiValue : multiValues) {
                multiValue.get();
            }
            break;
        case VERSION_HISTORY:
            List<Version> versions = foreignKey.getVersions();
            actualCount = versions.size();
            for (Version version : versions) {
                version.getValue();
                version.getModifiedDateObject();
            }
            break;
        case UNSUPPORTED:
            List<UnsupportedValue> unsupportedValues = foreignKey.getUnsupportedValues();
            actualCount = unsupportedValues.size();
            for (UnsupportedValue unsupportedValue : unsupportedValues) {
                unsupportedValue.getValues();
            }
            break;
        default:
            throw new IllegalStateException("Unknown complex type " + type);
        }
        if (actualCount != expectedCount) {
            throw new IllegalStateException("Complex foreign key count mismatch: " + actualCount + " != " + expectedCount);
        }
    }
}
