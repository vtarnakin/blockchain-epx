/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <graphene/protocol/transaction.hpp>
#include <graphene/protocol/block.hpp>
#include <graphene/protocol/exceptions.hpp>
#include <graphene/protocol/fee_schedule.hpp>
#include <graphene/protocol/pts_address.hpp>
#include <graphene/protocol/restriction_predicate.hpp>

#include <fc/io/raw.hpp>

namespace graphene { namespace protocol {

digest_type processed_transaction::merkle_digest()const
{
   digest_type::encoder enc;
   fc::raw::pack( enc, *this );
   return enc.result();
}

digest_type transaction::digest()const
{
   digest_type::encoder enc;
   fc::raw::pack( enc, *this );
   return enc.result();
}

digest_type transaction::sig_digest( const chain_id_type& chain_id )const
{
   digest_type::encoder enc;
   fc::raw::pack( enc, chain_id );
   fc::raw::pack( enc, *this );
   return enc.result();
}

void transaction::validate() const
{
   FC_ASSERT( operations.size() > 0, "A transaction must have at least one operation", ("trx",*this) );
   for( const auto& op : operations )
      operation_validate(op);
}

uint64_t transaction::get_packed_size() const
{
   return fc::raw::pack_size(*this);
}

const transaction_id_type& transaction::id() const
{
   auto h = digest();
   memcpy(_tx_id_buffer._hash, h._hash, std::min(sizeof(_tx_id_buffer), sizeof(h)));
   return _tx_id_buffer;
}

const signature_type& graphene::protocol::signed_transaction::sign(const private_key_type& key, const chain_id_type& chain_id)
{
   digest_type h = sig_digest( chain_id );
   signatures.push_back(key.sign_compact(h));
   return signatures.back();
}

signature_type graphene::protocol::signed_transaction::sign(const private_key_type& key, const chain_id_type& chain_id)const
{
   digest_type::encoder enc;
   fc::raw::pack( enc, chain_id );
   fc::raw::pack( enc, *this );
   return key.sign_compact(enc.result());
}

void transaction::set_expiration( fc::time_point_sec expiration_time )
{
    expiration = expiration_time;
}

void transaction::set_reference_block( const block_id_type& reference_block )
{
   ref_block_num = boost::endian::endian_reverse(reference_block._hash[0].value());
   ref_block_prefix = reference_block._hash[1].value();
}

void transaction::get_required_authorities( flat_set<account_id_type>& active,
                                            flat_set<account_id_type>& owner,
                                            vector<authority>& other,
                                            bool ignore_custom_operation_required_auths )const
{
   for( const auto& op : operations )
      operation_get_required_authorities( op, active, owner, other, ignore_custom_operation_required_auths );
   for( const auto& account : owner )
      active.erase( account );
}


const flat_set<public_key_type> empty_keyset;

struct sign_state
{
      /** returns true if we have a signature for this key or can
       * produce a signature for this key, else returns false.
       */
      bool signed_by( const public_key_type& k )
      {
         auto itr = provided_signatures.find(k);
         if( itr == provided_signatures.end() )
         {
            auto pk = available_keys.find(k);
            if( pk  != available_keys.end() )
               return provided_signatures[k] = true;
            return false;
         }
         return itr->second = true;
      }

      optional<map<address,public_key_type>> available_address_sigs;
      optional<map<address,public_key_type>> provided_address_sigs;

      bool signed_by( const address& a ) {
         if( !available_address_sigs ) {
            available_address_sigs = std::map<address,public_key_type>();
            provided_address_sigs = std::map<address,public_key_type>();
            for( auto& item : available_keys ) {
             (*available_address_sigs)[ address(pts_address(item, false, 56) ) ] = item;
             (*available_address_sigs)[ address(pts_address(item, true, 56) ) ] = item;
             (*available_address_sigs)[ address(pts_address(item, false, 0) ) ] = item;
             (*available_address_sigs)[ address(pts_address(item, true, 0) ) ] = item;
             (*available_address_sigs)[ address(item) ] = item;
            }
            for( auto& item : provided_signatures ) {
             (*provided_address_sigs)[ address(pts_address(item.first, false, 56) ) ] = item.first;
             (*provided_address_sigs)[ address(pts_address(item.first, true, 56) ) ] = item.first;
             (*provided_address_sigs)[ address(pts_address(item.first, false, 0) ) ] = item.first;
             (*provided_address_sigs)[ address(pts_address(item.first, true, 0) ) ] = item.first;
             (*provided_address_sigs)[ address(item.first) ] = item.first;
            }
         }
         auto itr = provided_address_sigs->find(a);
         if( itr == provided_address_sigs->end() )
         {
            auto aitr = available_address_sigs->find(a);
            if( aitr != available_address_sigs->end() ) {
               auto pk = available_keys.find(aitr->second);
               if( pk != available_keys.end() )
                  return provided_signatures[aitr->second] = true;
               return false;
            }
         }
         return provided_signatures[itr->second] = true;
      }

      bool check_authority( account_id_type id )
      {
         if( approved_by.find(id) != approved_by.end() ) return true;
         return check_authority( get_active(id) ) || ( allow_non_immediate_owner && check_authority( get_owner(id) ) );
      }

      /**
       *  Checks to see if we have signatures of the active authorites of
       *  the accounts specified in authority or the keys specified.
       */
      bool check_authority( const authority* au, uint32_t depth = 0 )
      {
         if( au == nullptr ) return false;
         const authority& auth = *au;

         uint32_t total_weight = 0;
         for( const auto& k : auth.key_auths )
            if( signed_by( k.first ) )
            {
               total_weight += k.second;
               if( total_weight >= auth.weight_threshold )
                  return true;
            }

         for( const auto& k : auth.address_auths )
            if( signed_by( k.first ) )
            {
               total_weight += k.second;
               if( total_weight >= auth.weight_threshold )
                  return true;
            }

         for( const auto& a : auth.account_auths )
         {
            if( approved_by.find(a.first) == approved_by.end() )
            {
               if( depth == max_recursion )
                  continue;
               if( check_authority( get_active( a.first ), depth+1 )
                     || ( allow_non_immediate_owner && check_authority( get_owner( a.first ), depth+1 ) ) )
               {
                  approved_by.insert( a.first );
                  total_weight += a.second;
                  if( total_weight >= auth.weight_threshold )
                     return true;
               }
            }
            else
            {
               total_weight += a.second;
               if( total_weight >= auth.weight_threshold )
                  return true;
            }
         }
         return total_weight >= auth.weight_threshold;
      }

      bool remove_unused_signatures()
      {
         vector<public_key_type> remove_sigs;
         for( const auto& sig : provided_signatures )
            if( !sig.second ) remove_sigs.push_back( sig.first );

         for( auto& sig : remove_sigs )
            provided_signatures.erase(sig);

         return remove_sigs.size() != 0;
      }

      sign_state( const flat_set<public_key_type>& sigs,
                  const std::function<const authority*(account_id_type)>& active,
                  const std::function<const authority*(account_id_type)>& owner,
                  bool allow_owner,
                  uint32_t max_recursion_depth = GRAPHENE_MAX_SIG_CHECK_DEPTH,
                  const flat_set<public_key_type>& keys = empty_keyset )
      :  get_active(active),
         get_owner(owner),
         allow_non_immediate_owner(allow_owner),
         max_recursion(max_recursion_depth),
         available_keys(keys)
      {
         for( const auto& key : sigs )
            provided_signatures[ key ] = false;
         approved_by.insert( GRAPHENE_TEMP_ACCOUNT  );
      }

      const std::function<const authority*(account_id_type)>& get_active;
      const std::function<const authority*(account_id_type)>& get_owner;

      const bool                       allow_non_immediate_owner;
      const uint32_t                   max_recursion;
      const flat_set<public_key_type>& available_keys;

      flat_map<public_key_type,bool>   provided_signatures;
      flat_set<account_id_type>        approved_by;
};


void verify_authority( const vector<operation>& ops, const flat_set<public_key_type>& sigs,
                       const std::function<const authority*(account_id_type)>& get_active,
                       const std::function<const authority*(account_id_type)>& get_owner,
                       const custom_authority_lookup& get_custom,
                       bool allow_non_immediate_owner,
                       bool ignore_custom_operation_required_auths,
                       uint32_t max_recursion_depth,
                       bool  allow_committee,
                       const flat_set<account_id_type>& active_aprovals,
                       const flat_set<account_id_type>& owner_approvals )
{
   rejected_predicate_map rejected_custom_auths;
   try {
   flat_set<account_id_type> required_active;
   flat_set<account_id_type> required_owner;
   vector<authority> other;

   sign_state s( sigs, get_active, get_owner, allow_non_immediate_owner, max_recursion_depth );
   for( auto& id : active_aprovals )
      s.approved_by.insert( id );
   for( auto& id : owner_approvals )
      s.approved_by.insert( id );

   auto approved_by_custom_authority = [&s, &rejected_custom_auths, get_custom = std::move(get_custom)](
           account_id_type account,
           operation op ) mutable {
      auto viable_custom_auths = get_custom( account, op, &rejected_custom_auths );
      for( const auto& auth : viable_custom_auths )
         if( s.check_authority( &auth ) ) return true;
      return false;
   };

   for( const auto& op : ops ) {
      flat_set<account_id_type> operation_required_active;
      operation_get_required_authorities( op, operation_required_active, required_owner, other,
                                          ignore_custom_operation_required_auths );

      auto itr = operation_required_active.begin();
      while ( itr != operation_required_active.end() ) {
         if ( approved_by_custom_authority( *itr, op ) )
            itr = operation_required_active.erase( itr );
         else
            ++itr;
      }

      required_active.insert( operation_required_active.begin(), operation_required_active.end() );
   }

   if( !allow_committee )
      GRAPHENE_ASSERT( required_active.find(GRAPHENE_COMMITTEE_ACCOUNT) == required_active.end(),
                       invalid_committee_approval, "Committee account may only propose transactions" );

   for( const auto& auth : other )
   {
      GRAPHENE_ASSERT( s.check_authority(&auth), tx_missing_other_auth, "Missing Authority", ("auth",auth)("sigs",sigs) );
   }

   // fetch all of the top level authorities
   for( auto id : required_owner )
   {
      GRAPHENE_ASSERT( owner_approvals.find(id) != owner_approvals.end() ||
                       s.check_authority(get_owner(id)),
                       tx_missing_owner_auth, "Missing Owner Authority ${id}", ("id",id)("auth",*get_owner(id)) );
   }

   for( auto id : required_active )
   {
      GRAPHENE_ASSERT( s.check_authority(id) ||
                       s.check_authority(get_owner(id)),
                       tx_missing_active_auth, "Missing Active Authority ${id}",
                       ("id",id)("auth",*get_active(id))("owner",*get_owner(id)) );
   }

   GRAPHENE_ASSERT(
      !s.remove_unused_signatures(),
      tx_irrelevant_sig,
      "Unnecessary signature(s) detected"
      );
} FC_CAPTURE_AND_RETHROW( (rejected_custom_auths)(ops)(sigs) ) }


const flat_set<public_key_type>& signed_transaction::get_signature_keys( const chain_id_type& chain_id )const
{ try {
   auto d = sig_digest( chain_id );
   flat_set<public_key_type> result;
   for( const auto&  sig : signatures )
   {
      GRAPHENE_ASSERT(
         result.insert( fc::ecc::public_key(sig,d) ).second,
            tx_duplicate_sig,
            "Duplicate Signature detected" );
   }
   _signees = std::move( result );
   return _signees;
} FC_CAPTURE_AND_RETHROW() }


set<public_key_type> signed_transaction::get_required_signatures( const chain_id_type& chain_id,
                                                                  const flat_set<public_key_type>& available_keys,
                                                                  const std::function<const authority*(account_id_type)>& get_active,
                                                                  const std::function<const authority*(account_id_type)>& get_owner,
                                                                  bool allow_non_immediate_owner,
                                                                  bool ignore_custom_operation_required_authorities,
                                                                  uint32_t max_recursion_depth )const
{
   flat_set<account_id_type> required_active;
   flat_set<account_id_type> required_owner;
   vector<authority> other;
   get_required_authorities( required_active, required_owner, other, ignore_custom_operation_required_authorities );

   const flat_set<public_key_type>& signature_keys = get_signature_keys(chain_id);
   sign_state s( signature_keys, get_active, get_owner, allow_non_immediate_owner, max_recursion_depth, available_keys );

   for( const auto& auth : other )
      s.check_authority( &auth );
   for( auto& owner : required_owner )
      s.check_authority( get_owner( owner ) );
   for( auto& active : required_active )
      s.check_authority( active ) || s.check_authority( get_owner( active ) );

   s.remove_unused_signatures();

   set<public_key_type> result;

   for( auto& provided_sig : s.provided_signatures )
      if( available_keys.find( provided_sig.first ) != available_keys.end()
            && signature_keys.find( provided_sig.first ) == signature_keys.end() )
         result.insert( provided_sig.first );

   return result;
}

set<public_key_type> signed_transaction::minimize_required_signatures(
         const chain_id_type& chain_id,
         const flat_set<public_key_type>& available_keys,
         const std::function<const authority*(account_id_type)>& get_active,
         const std::function<const authority*(account_id_type)>& get_owner,
         const custom_authority_lookup &get_custom,
         bool allow_non_immediate_owner,
         bool ignore_custom_operation_required_auths,
         uint32_t max_recursion )const
{
   set< public_key_type > s = get_required_signatures( chain_id, available_keys, get_active, get_owner,
                                                       allow_non_immediate_owner,
                                                       ignore_custom_operation_required_auths, max_recursion );
   flat_set< public_key_type > result( s.begin(), s.end() );

   for( const public_key_type& k : s )
   {
      result.erase( k );
      try
      {
         graphene::protocol::verify_authority( operations, result, get_active, get_owner, get_custom,
                                               allow_non_immediate_owner,ignore_custom_operation_required_auths,
                                               max_recursion );
         continue;  // element stays erased if verify_authority is ok
      }
      catch( const tx_missing_owner_auth& e ) {}
      catch( const tx_missing_active_auth& e ) {}
      catch( const tx_missing_other_auth& e ) {}
      result.insert( k );
   }
   return set<public_key_type>( result.begin(), result.end() );
}

const transaction_id_type& precomputable_transaction::id()const
{
   if( 0 == _tx_id_buffer._hash[0].value() )
      transaction::id();
   return _tx_id_buffer;
}

void precomputable_transaction::validate() const
{
   if( _validated ) return;
   transaction::validate();
   _validated = true;
}

uint64_t precomputable_transaction::get_packed_size()const
{
   if( _packed_size == 0 )
      _packed_size = transaction::get_packed_size();
   return _packed_size;
}

const flat_set<public_key_type>& precomputable_transaction::get_signature_keys( const chain_id_type& chain_id )const
{
   // Strictly we should check whether the given chain ID is same as the one used to initialize the `signees` field.
   // However, we don't pass in another chain ID so far, for better performance, we skip the check.
   if( _signees.empty() )
      signed_transaction::get_signature_keys( chain_id );
   return _signees;
}

void signed_transaction::verify_authority( const chain_id_type& chain_id,
                                           const std::function<const authority*(account_id_type)>& get_active,
                                           const std::function<const authority*(account_id_type)>& get_owner,
                                           const custom_authority_lookup& get_custom,
                                           bool allow_non_immediate_owner,
                                           bool ignore_custom_operation_required_auths,
                                           uint32_t max_recursion )const
{ try {
   graphene::protocol::verify_authority( operations, get_signature_keys( chain_id ), get_active, get_owner,
                                         get_custom, allow_non_immediate_owner,
                                         ignore_custom_operation_required_auths, max_recursion );
} FC_CAPTURE_AND_RETHROW( (*this) ) }

} } // graphene::protocol

GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::transaction)
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::signed_transaction)
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::precomputable_transaction)
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::processed_transaction)
