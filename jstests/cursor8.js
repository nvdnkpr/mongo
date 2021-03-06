// This should get skipped when testing replication.

db.dropDatabase();
t = db.cursor8;
t.save( {} );
t.save( {} );
t.save( {} );

function test( want , msg ){
    var res = db.runCommand( { cursorInfo:1 } );
    assert.eq( want , res.clientCursors_size , msg + " " + tojson( res ) );
}

test( 0 , "A1" );
assert.eq( 3 , t.find().count() , "A2" );
assert.eq( 3 , t.find( {} ).count() , "A3" );
assert.eq( 2, t.find( {} ).limit( 2 ).itcount() , "A4" );
test( 1 , "B1" );

