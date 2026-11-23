import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Relationship;
import io.github.spannm.jackcess.RelationshipBuilder;
import io.github.spannm.jackcess.TableBuilder;
import java.nio.file.Path;
import java.util.List;

/**
 * Creates the foreign-key fixtures used by tests/foreign_key_test.cpp.
 *
 * <p>The fixtures hold five composite-key relationships over nine tables:
 *
 * <ul>
 *   <li>{@code ParentChild}: referential integrity plus cascade updates and deletes.
 *   <li>{@code StrictParentChild}: referential integrity only, so the C++ writer has to reject
 *       dependent-parent changes instead of cascading them.
 *   <li>{@code NullOnDelete}: referential integrity plus cascade-to-NULL on delete.
 *   <li>{@code ChainTopMiddle} / {@code ChainMiddleLeaf}: a three-level cascading chain that
 *       forces the writer to cascade recursively.
 * </ul>
 *
 * <p>In each relationship Jet stores the parent-side alias as an extra logical index over the
 * parent's primary index data, so one physical index serves the primary key and the foreign key
 * description.
 */
public final class JackcessForeignKeyProbe {
    private static void compositePair(Database db, String parent, String child) throws Exception {
        new TableBuilder(parent)
            .addColumn(new ColumnBuilder("Region", DataType.LONG))
            .addColumn(new ColumnBuilder("Code", DataType.LONG))
            .withPrimaryKey("Region", "Code")
            .toTable(db);
        new TableBuilder(child)
            .addColumn(new ColumnBuilder("ForeignRegion", DataType.LONG))
            .addColumn(new ColumnBuilder("ForeignCode", DataType.LONG))
            .toTable(db);
    }

    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[0]);
        try (var db = new DatabaseBuilder().withPath(path)
                .withFileFormat(Database.FileFormat.valueOf(args[1])).create()) {
            compositePair(db, "Parent", "Child");
            new RelationshipBuilder("Parent", "Child")
                .withName("ParentChild")
                .addColumns("Region", "ForeignRegion")
                .addColumns("Code", "ForeignCode")
                .withReferentialIntegrity().withCascadeUpdates().withCascadeDeletes()
                .withJoinType(Relationship.JoinType.LEFT_OUTER)
                .toRelationship(db);

            compositePair(db, "StrictParent", "StrictChild");
            new RelationshipBuilder("StrictParent", "StrictChild")
                .withName("StrictParentChild")
                .addColumns("Region", "ForeignRegion")
                .addColumns("Code", "ForeignCode")
                .withReferentialIntegrity()
                .withJoinType(Relationship.JoinType.INNER)
                .toRelationship(db);

            compositePair(db, "NullParent", "NullChild");
            new RelationshipBuilder("NullParent", "NullChild")
                .withName("NullOnDelete")
                .addColumns("Region", "ForeignRegion")
                .addColumns("Code", "ForeignCode")
                .withReferentialIntegrity().withCascadeNullOnDelete()
                .withJoinType(Relationship.JoinType.INNER)
                .toRelationship(db);

            // Key types the writer encodes through the same index-entry path as ordinary index
            // maintenance, so the foreign key lookups have to agree with those encodings.
            new TableBuilder("TextParent")
                .addColumn(new ColumnBuilder("Code", DataType.TEXT))
                .withPrimaryKey("Code")
                .toTable(db);
            new TableBuilder("TextChild")
                .addColumn(new ColumnBuilder("ForeignCode", DataType.TEXT))
                .toTable(db);
            new RelationshipBuilder("TextParent", "TextChild")
                .withName("TextParentChild")
                .addColumns("Code", "ForeignCode")
                .withReferentialIntegrity()
                .withJoinType(Relationship.JoinType.INNER)
                .toRelationship(db);

            new TableBuilder("BinParent")
                .addColumn(new ColumnBuilder("Code", DataType.BINARY))
                .withPrimaryKey("Code")
                .toTable(db);
            new TableBuilder("BinChild")
                .addColumn(new ColumnBuilder("ForeignCode", DataType.BINARY))
                .toTable(db);
            new RelationshipBuilder("BinParent", "BinChild")
                .withName("BinParentChild")
                .addColumns("Code", "ForeignCode")
                .withReferentialIntegrity()
                .withJoinType(Relationship.JoinType.INNER)
                .toRelationship(db);

            new TableBuilder("GuidParent")
                .addColumn(new ColumnBuilder("Id", DataType.GUID))
                .withPrimaryKey("Id")
                .toTable(db);
            new TableBuilder("GuidChild")
                .addColumn(new ColumnBuilder("ForeignId", DataType.GUID))
                .toTable(db);
            new RelationshipBuilder("GuidParent", "GuidChild")
                .withName("GuidParentChild")
                .addColumns("Id", "ForeignId")
                .withReferentialIntegrity()
                .withJoinType(Relationship.JoinType.INNER)
                .toRelationship(db);

            // A three-level chain: the middle table's foreign key columns are also its primary
            // key, so cascading a top key change has to cascade again from the middle table.
            new TableBuilder("ChainTop")
                .addColumn(new ColumnBuilder("Region", DataType.LONG))
                .addColumn(new ColumnBuilder("Code", DataType.LONG))
                .withPrimaryKey("Region", "Code")
                .toTable(db);
            new TableBuilder("ChainMiddle")
                .addColumn(new ColumnBuilder("ForeignRegion", DataType.LONG))
                .addColumn(new ColumnBuilder("ForeignCode", DataType.LONG))
                .withPrimaryKey("ForeignRegion", "ForeignCode")
                .toTable(db);
            new RelationshipBuilder("ChainTop", "ChainMiddle")
                .withName("ChainTopMiddle")
                .addColumns("Region", "ForeignRegion")
                .addColumns("Code", "ForeignCode")
                .withReferentialIntegrity().withCascadeUpdates().withCascadeDeletes()
                .withJoinType(Relationship.JoinType.INNER)
                .toRelationship(db);
            new TableBuilder("ChainLeaf")
                .addColumn(new ColumnBuilder("ForeignRegion", DataType.LONG))
                .addColumn(new ColumnBuilder("ForeignCode", DataType.LONG))
                .toTable(db);
            new RelationshipBuilder("ChainMiddle", "ChainLeaf")
                .withName("ChainMiddleLeaf")
                .addColumns("ForeignRegion", "ForeignRegion")
                .addColumns("ForeignCode", "ForeignCode")
                .withReferentialIntegrity().withCascadeUpdates().withCascadeDeletes()
                .withJoinType(Relationship.JoinType.INNER)
                .toRelationship(db);
        }
        try (var db = new DatabaseBuilder().withPath(path).withReadOnly(true).open()) {
            List<Relationship> relations = db.getRelationships();
            if (relations.size() != 8) {
                throw new AssertionError(relations);
            }
            var cascading = relations.stream().filter(rel -> rel.getName().equals("ParentChild"))
                .findFirst().orElseThrow();
            var strict = relations.stream().filter(rel -> rel.getName().equals("StrictParentChild"))
                .findFirst().orElseThrow();
            var nulling = relations.stream().filter(rel -> rel.getName().equals("NullOnDelete"))
                .findFirst().orElseThrow();
            var chain = relations.stream().filter(rel -> rel.getName().equals("ChainMiddleLeaf"))
                .findFirst().orElseThrow();
            if (cascading.getFromColumns().size() != 2 ||
                !cascading.hasReferentialIntegrity() ||
                !cascading.cascadeUpdates() || !cascading.cascadeDeletes() ||
                strict.getToColumns().size() != 2 ||
                !strict.hasReferentialIntegrity() ||
                strict.cascadeUpdates() || strict.cascadeDeletes() ||
                !nulling.hasReferentialIntegrity() || nulling.cascadeUpdates() ||
                !nulling.cascadeNullOnDelete() ||
                !chain.hasReferentialIntegrity() || !chain.cascadeDeletes() ||
                relations.stream().noneMatch(rel -> rel.getName().equals("TextParentChild")) ||
                relations.stream().noneMatch(rel -> rel.getName().equals("BinParentChild")) ||
                relations.stream().noneMatch(rel -> rel.getName().equals("GuidParentChild"))) {
                throw new AssertionError(relations);
            }
        }
    }
}
